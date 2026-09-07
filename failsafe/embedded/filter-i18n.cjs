#!/usr/bin/env node

/*
 * Strip unused language blocks from an i18n dictionary at build time.
 *
 * Both embedded Web UIs ship a single i18n dictionary holding every
 * translation.  Each language costs roughly 5 KiB of compressed data, so
 * boards that only need one or two languages can drop the rest here.
 *
 * The dictionary is edited as text: a language block is a top-level
 * member whose key looks like a language code ("en", "zh-cn", "zh",
 * "ru", ...) followed by an object literal, and ends at the first line
 * with the same indentation holding "}" or "},".  Nothing else in the
 * file is touched, so the transformation cannot change any behaviour
 * other than which languages are present.
 *
 * usage: node filter-i18n.cjs --langs="en zh zh-cn ru" <input.js> <output.js>
 *        (the language list may be comma or whitespace separated)
 */

const fs = require("fs");
const path = require("path");

function fail(message) {
    console.error(`[failsafe-i18n] ${message}`);
    process.exit(1);
}

const args = process.argv.slice(2);
let langs = null;
const positional = [];

for (const arg of args) {
    if (arg.startsWith("--langs="))
        langs = arg.slice("--langs=".length).split(/[,\s]+/).filter(Boolean);
    else
        positional.push(arg);
}

const [inputPath, outputPath] = positional;

if (!inputPath || !outputPath || !langs || !langs.length)
    fail("usage: node filter-i18n.cjs --langs=en,zh <input.js> <output.js>");

let source;
try {
    source = fs.readFileSync(inputPath, "utf8");
} catch (error) {
    fail(`read failed for ${inputPath}: ${error.message}`);
}

/* "en", "zh-cn", "zh", "ru", ... optionally quoted */
const LANG = "[a-z]{2}(?:-[a-zA-Z]{2,4})?";
const keyRe = new RegExp(
    `^(\\s*)(?:"(${LANG})"|'(${LANG})'|(${LANG}))\\s*:\\s*\\{\\s*$`);

const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

const lines = source.split(/\r?\n/);
const out = [];
const kept = [];
const removed = [];

let i = 0;
while (i < lines.length) {
    const match = lines[i].match(keyRe);
    let end = -1;

    if (match) {
        const indent = match[1];
        const name = match[2] || match[3] || match[4];
        const closeRe = new RegExp(`^${escapeRe(indent)}\\},?\\s*$`);

        for (let j = i + 1; j < lines.length; j++) {
            if (closeRe.test(lines[j])) {
                end = j;
                break;
            }
        }

        if (end >= 0) {
            if (langs.includes(name)) {
                out.push(...lines.slice(i, end + 1));
                kept.push(name);
            } else {
                removed.push(name);
            }
            i = end + 1;
            continue;
        }
    }

    out.push(lines[i]);
    i += 1;
}

if (!kept.length)
    fail(`no requested language (${langs.join(",")}) found in ${inputPath}`);

try {
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    fs.writeFileSync(outputPath, out.join("\n"), "utf8");
} catch (error) {
    fail(`write failed for ${outputPath}: ${error.message}`);
}

console.log(`[i18n] ${path.basename(inputPath)}: kept ${kept.join(",") || "-"}` +
    (removed.length ? `, dropped ${removed.join(",")}` : ""));
