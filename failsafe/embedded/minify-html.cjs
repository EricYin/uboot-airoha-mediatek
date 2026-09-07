#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const { minify } = require("html-minifier-terser");

function fail(message) {
    console.error(`[failsafe-minify-html] ${message}`);
    process.exit(1);
}

const [inputPath, outputPath] = process.argv.slice(2);

if (!inputPath || !outputPath) {
    fail("usage: node minify-html.cjs <input.html> <output.html>");
}

let source;
try {
    source = fs.readFileSync(inputPath, "utf8");
} catch (error) {
    fail(`read failed for ${inputPath}: ${error.message}`);
}

const options = {
    collapseWhitespace: true,
    conservativeCollapse: true,
    continueOnParseError: false,
    keepClosingSlash: true,
    minifyCSS: false,
    minifyJS: false,
    removeComments: true,
    removeRedundantAttributes: true,
    removeScriptTypeAttributes: true,
    removeStyleLinkTypeAttributes: true,
    sortAttributes: true,
    sortClassName: false,
    useShortDoctype: true,
};

(async () => {
    const result = await minify(source, options);

    try {
        fs.mkdirSync(path.dirname(outputPath), { recursive: true });
        fs.writeFileSync(outputPath, result || "", "utf8");
    } catch (error) {
        fail(`write failed for ${outputPath}: ${error.message}`);
    }
})().catch((error) => {
    fail(`minify failed for ${inputPath}: ${error.message || error}`);
});
