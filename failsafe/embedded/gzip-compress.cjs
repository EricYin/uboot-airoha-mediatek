#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

function fail(message) {
    console.error(`[failsafe-compress] ${message}`);
    process.exit(1);
}

/*
 * usage: node gzip-compress.cjs [--gzip|--none] <input> <output>
 *
 * --gzip    (default) gzip stream, served as Content-Encoding: gzip
 * --none    copy the file unchanged, served without Content-Encoding
 */
const args = process.argv.slice(2);
let algo = "gzip";
const positional = [];

for (const arg of args) {
    if (arg === "--gzip" || arg === "--none")
        algo = arg.slice(2);
    else
        positional.push(arg);
}

const [inputPath, outputPath] = positional;

if (!inputPath || !outputPath) {
    fail("usage: node gzip-compress.cjs [--gzip|--none] <input> <output>");
}

let source;
try {
    source = fs.readFileSync(inputPath);
} catch (error) {
    fail(`read failed for ${inputPath}: ${error.message}`);
}

let compressed;
try {
    if (algo === "none") {
        compressed = source;
    } else {
        compressed = zlib.gzipSync(source, { level: zlib.constants.Z_BEST_COMPRESSION });
    }
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
console.log(`[${algo}] ${path.basename(inputPath)}: ${source.length} -> ${compressed.length} bytes (${ratio}% reduction)`);
