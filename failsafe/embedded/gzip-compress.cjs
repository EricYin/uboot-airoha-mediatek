#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

function fail(message) {
    console.error(`[failsafe-gzip] ${message}`);
    process.exit(1);
}

const [inputPath, outputPath] = process.argv.slice(2);

if (!inputPath || !outputPath) {
    fail("usage: node gzip-compress.cjs <input> <output>");
}

let source;
try {
    source = fs.readFileSync(inputPath);
} catch (error) {
    fail(`read failed for ${inputPath}: ${error.message}`);
}

let compressed;
try {
    compressed = zlib.gzipSync(source, { level: zlib.constants.Z_BEST_COMPRESSION });
} catch (error) {
    fail(`compression failed for ${inputPath}: ${error.message}`);
}

if (!compressed || compressed.length === 0) {
    fail(`compression produced no output for ${inputPath}`);
}

try {
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    fs.writeFileSync(outputPath, compressed);
} catch (error) {
    fail(`write failed for ${outputPath}: ${error.message}`);
}

const ratio = ((1 - compressed.length / source.length) * 100).toFixed(1);
console.log(`[gzip] ${path.basename(inputPath)}: ${source.length} -> ${compressed.length} bytes (${ratio}% reduction)`);
