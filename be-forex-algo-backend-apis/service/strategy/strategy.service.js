const { redis } = require("../../config/redis.config");
const { logger } = require("../../helpers");

let RANGE = 5;
let GAP = 2;
let ECLIPSE_BUFFER = 0.30;

function floorCheckpoint(price) {
    return Math.floor(price);
}

function generateCheckpointRange(cp) {
    const prevs = Array.from({ length: RANGE }, (_, i) => cp - GAP * (i + 1)).reverse();
    const nexts = Array.from({ length: RANGE }, (_, i) => cp + GAP * (i + 1));
    return { prevs, nexts };
}

function findClosestLevels(price, prevs, nexts) {
    if (nexts.includes(price)) return { cp: price, direction: "BUY" };
    if (prevs.includes(price)) return { cp: price, direction: "SELL" };

    if (price < Math.min(...prevs)) return { cp: prevs[0], direction: "SELL" };
    if (price > Math.max(...nexts)) return { cp: nexts[nexts.length - 1], direction: "BUY" };

    const lowerNext = nexts.filter(n => n < price).at(-1);
    if (lowerNext) return { cp: lowerNext, direction: "BUY" };

    const upperPrev = prevs.filter(p => p > price)[0];
    if (upperPrev) return { cp: upperPrev, direction: "SELL" };

    return { cp: null, direction: null };
};

async function sendTrade(symbol, price, direction) {
    try {
        const [symbolConfig, checkpoint] = await Promise.all([
            redis.hgetall(`symbol_config:${symbol}`),
            redis.hgetall(`checkpoint:${symbol}`)
        ]);

        const GAP = parseFloat(symbolConfig.GAP) || 0;
        const ECLIPSE_BUFFER = parseFloat(symbolConfig.ECLIPSE_BUFFER) || 0;
        const volume = parseFloat(symbolConfig.volume) || 0.1;

        const nonce = Math.floor(Math.random() * 1000000).toString(36).substring(2, 10);

        const message = {
            type: "trade",
            symbol: symbolConfig.symbol || symbol,
            action: direction,
            price,
            GAP,
            ECLIPSE_BUFFER,
            checkpoint: parseInt(checkpoint.current) || 0,
            initialTraded: checkpoint.initialTraded === "1",
            direction: checkpoint.direction || "",
            nonce,
            volume
        };

        const { sendMessageToDLL } = require("../tcp/tcpClient.service");
        sendMessageToDLL(message);
    } catch (err) {
        console.error("❌ sendTrade Error:", err);
    }
}

async function handlePriceUpdate(data) {
    try {
        const parsed = typeof data === "string" ? JSON.parse(data) : data;
        const { symbol, bid, ask, GAP: dynamicGAP, ECLIPSE_BUFFER: dynamicEclipseBuffer } = parsed;

        if (!symbol || typeof bid !== "number") return;

        if (typeof dynamicGAP === "number" && dynamicGAP > 0) GAP = dynamicGAP;
        if (typeof dynamicEclipseBuffer === "number" && dynamicEclipseBuffer > 0) ECLIPSE_BUFFER = dynamicEclipseBuffer;

        const price = parseFloat(bid);
        const newCP = floorCheckpoint(price);
        const redisKey = `checkpoint:${symbol}`;

        let redisCheckpoint = await redis.hgetall(redisKey);
        if (!redisCheckpoint || Object.keys(redisCheckpoint).length === 0) {
            await redis.hset(redisKey, {
                current: newCP,
                direction: "",
                initialTraded: 0
            });
            return;
        }

        const current = parseInt(redisCheckpoint.current);
        const direction = redisCheckpoint.direction;
        const initialTraded = redisCheckpoint.initialTraded === "1";

        const { prevs, nexts } = generateCheckpointRange(current);
        const updateCheckpoint = async (updatedCP, newDirection, shouldTrade = true) => {
            await redis.hset(redisKey, {
                current: updatedCP,
                direction: newDirection,
                initialTraded: 1
            });
            const { prevs, nexts } = generateCheckpointRange(updatedCP);
            const next = nexts[0];
            const prev = prevs[prevs.length - 1];
            logger.info(`🔁 ${symbol}: ${price} | Checkpoint Updated | Current: ${updatedCP} | Prev: ${prev} | Next: ${next}`);
            if (shouldTrade) {
                const tradePrice = newDirection === "BUY" ? parseFloat(ask) : price;
                sendTrade(symbol, tradePrice, newDirection);
            } else {
                const tradePrice = newDirection === "BUY" ? parseFloat(ask) : price;
                logger.info(`⏭️ ${symbol}: ${tradePrice} | Skip re-entry | Current: ${updatedCP}`);
            }
        };

        // Initial Trade
        if (!initialTraded) {
            if (Math.abs(price - current) >= ECLIPSE_BUFFER) {
                const initialDirection = price > current ? "BUY" : "SELL";
                const tradePrice = initialDirection === "BUY" ? parseFloat(ask) : price;
                const initialCP = floorCheckpoint(tradePrice);
                await redis.hset(redisKey, {
                    current: initialCP,
                    direction: initialDirection,
                    initialTraded: 1
                });
                await redis.hset(`symbol_config:${symbol}`, {
                    symbol,
                    GAP: parseFloat(dynamicGAP) || 0,
                    ECLIPSE_BUFFER: 0
                });
                logger.info(`🥇 ${symbol}: ${tradePrice} | Initial Trade | Current: ${initialCP} | Prev: ${initialCP - GAP} | Next: ${initialCP + GAP}`);
                sendTrade(symbol, tradePrice, initialDirection);
            }

            return;
        };

        if (!initialTraded) return;

        //! Trade Logic
        const flooredPrice = floorCheckpoint(price);
        const { cp: closestCP, direction: cpDirection } = findClosestLevels(flooredPrice, prevs, nexts);

        if (direction === "BUY") {

            if (closestCP && cpDirection === "BUY" && closestCP > current) {
                logger.warn('UPDATE CP BUY: Price >= Next CP')
                await updateCheckpoint(closestCP, "BUY", false); // No re-entry
            }

            else if (flooredPrice < current) {
                logger.warn('EXIT BUY : ENTER SELL');
                await updateCheckpoint(flooredPrice, "SELL", true); // Reverse trade
            }

        } else if (direction === "SELL") {

            if (closestCP && cpDirection === "SELL" && closestCP < current) {
                logger.warn('UPDATE CP SELL: Price <= Prev CP');
                await updateCheckpoint(closestCP, "SELL", false); // No re-entry
            }

            else if (flooredPrice > current) {
                logger.warn('EXIT SELL : ENTER BUY');
                await updateCheckpoint(flooredPrice, "BUY", true); // Reverse trade
            }

        }

    } catch (err) {
        console.error("❌ handlePriceUpdate Error:", err);
        console.error("Offending message:", data);
    }
}

module.exports = { handlePriceUpdate };