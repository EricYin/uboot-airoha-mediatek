#!/usr/bin/env node

/*
 * Strip what the Web UI cannot show from an i18n dictionary at build time.
 *
 * Both embedded Web UIs ship a single i18n dictionary holding every
 * translation.  Two kinds of entries can never reach the user and are
 * dropped here:
 *
 *   - languages that are not configured (--langs);
 *   - the strings of pages that are not built into this firmware
 *     (--drop-ns, used by the bootstrap UI whose pages are separate
 *     assets each gated by its own Kconfig option).
 *
 * The dictionary is edited as text, so the transformation cannot change
 * any behaviour other than which entries are present:
 *
 *   - a language block is a top-level member whose key looks like a
 *     language code ("en", "zh-cn", "zh", "ru", ...) followed by an object
 *     literal; it ends at the first line with the same indentation holding
 *     "}" or "},";
 *   - a key is a quoted "namespace.name" member on a line of its own
 *     (every entry of these dictionaries is one line, values included).
 *
 * "--drop-ns=*" drops every namespace at once and leaves the decision to the
 * assets alone; it is meant for the small UI variant whose pages are all
 * "not built" as far as this option is concerned.
 *
 * A key is *kept* whenever an embedded asset still mentions it, whatever
 * --drop-ns says:
 *
 *   - --assets lists the source files that are actually embedded; every
 *     quoted key-looking string in them counts as a reference.  This also
 *     covers keys that are passed around as values instead of being used
 *     in a t("...") call, e.g. createNavLink("/gpt.html", "nav.gpt", "gpt")
 *     - and it is why a key such as "gpt.err.too_big", used by the shared
 *     upload flow of main.js, survives the removal of the other "gpt.*"
 *     strings;
 *   - "prefix." + ... in an asset keeps the whole prefix subtree;
 *   - `${...}suffix` in an asset keeps every key ending with that suffix
 *     (main.js builds the page title as `${APP_STATE.page}.title`).
 *
 * Nothing is ever dropped because of the asset scan: it can only keep more
 * keys than --drop-ns asks for, so an unnoticed dynamic key costs a few
 * bytes, not a translation.
 *
 * usage: node filter-i18n.cjs --langs="en zh zh-cn" \
 *            [--drop-ns="ubi simg gpt"] [--assets="path/a.js path/b.html"] \
 *            <input.js> <output.js>
 *        (the lists may be comma or whitespace separated)
 */

const fs = require("fs");
const path = require("path");

function fail(message) {
    console.error(`[failsafe-i18n] ${message}`);
    process.exit(1);
}

const args = process.argv.slice(2);
let langs = null;
let dropNs = [];
let assets = [];
const positional = [];

for (const arg of args) {
    const value = (name) =>
        arg.slice(name.length).split(/[,\s]+/).filter(Boolean);

    if (arg.startsWith("--langs="))
        langs = value("--langs=");
    else if (arg.startsWith("--drop-ns="))
        dropNs = value("--drop-ns=");
    else if (arg.startsWith("--assets="))
        assets = value("--assets=");
    else
        positional.push(arg);
}

const [inputPath, outputPath] = positional;

if (!inputPath || !outputPath || !langs || !langs.length)
    fail("usage: node filter-i18n.cjs --langs=en,zh [--drop-ns=ubi] " +
        "[--assets=a.js,b.html] <input.js> <output.js>");

let source;
try {
    source = fs.readFileSync(inputPath, "utf8");
} catch (error) {
    fail(`read failed for ${inputPath}: ${error.message}`);
}

/* ------------------------------------------------------------------ */
/*  What the embedded assets still reference                           */
/* ------------------------------------------------------------------ */

const referenced = new Set();
const dynPrefixes = new Set();
const dynSuffixes = new Set();

/* A quoted string that looks like a dictionary key. */
const KEY_LITERAL =
    /["'`]([A-Za-z0-9_][A-Za-z0-9_.\-]*\.[A-Za-z0-9_.\-]+)["'`]/g;
/* data-i18n-attr="value:key" (or "a:k1;b:k2"). */
const KEY_ATTR = /data-i18n-attr=["']([^"']+)["']/g;
/* "prefix." + something - the subtree is built at runtime. */
const KEY_PREFIX = /["'`]([A-Za-z0-9_][A-Za-z0-9_]*\.)["'`]\s*\+/g;
/* `${...}suffix` - the key name is built at runtime. */
const KEY_SUFFIX = /\$\{[^}]*\}(\.[A-Za-z0-9_.\-]+)?/g;

for (const file of assets) {
    let text;

    try {
        text = fs.readFileSync(file, "utf8");
    } catch (error) {
        fail(`read failed for --assets entry ${file}: ${error.message}`);
    }

    for (const match of text.matchAll(KEY_LITERAL))
        referenced.add(match[1]);

    /* data-i18n-attr holds "attribute:key" pairs, which the literal scan
     * above cannot see (the quotes hold more than the key). */
    for (const match of text.matchAll(KEY_ATTR))
        for (const pair of match[1].split(";")) {
            const key = (pair.split(":")[1] || "").trim();

            if (key)
                referenced.add(key);
        }

    for (const match of text.matchAll(KEY_PREFIX))
        dynPrefixes.add(match[1]);

    for (const match of text.matchAll(KEY_SUFFIX))
        if (match[1])
            dynSuffixes.add(match[1]);
}

const isReferenced = (key) =>
    referenced.has(key) ||
    [...dynPrefixes].some((prefix) => key.startsWith(prefix)) ||
    [...dynSuffixes].some((suffix) => key.endsWith(suffix));

/* ------------------------------------------------------------------ */
/*  Edit the dictionary                                                */
/* ------------------------------------------------------------------ */

/* "en", "zh-cn", "zh", "ru", ... optionally quoted */
const LANG = "[a-z]{2}(?:-[a-zA-Z]{2,4})?";
const langRe = new RegExp(
    `^(\\s*)(?:"(${LANG})"|'(${LANG})'|(${LANG}))\\s*:\\s*\\{\\s*$`);
/* "namespace.name": value,   (the value stays on the same line) */
const keyRe = /^(\s*)"([A-Za-z0-9_][A-Za-z0-9_.\-]*)"\s*:/;

const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

const lines = source.split(/\r?\n/);
const out = [];
const keptLangs = [];
const droppedLangs = [];
const keptKeys = new Set();
const droppedKeys = new Map();   /* namespace -> count */
const vetoed = [];
let inKeptLang = false;

for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const langMatch = line.match(langRe);

    /* A language block: keep it or skip it as a whole. */
    if (langMatch) {
        const indent = langMatch[1];
        const name = langMatch[2] || langMatch[3] || langMatch[4];
        const closeRe = new RegExp(`^${escapeRe(indent)}\\},?\\s*$`);
        let end = -1;

        for (let j = i + 1; j < lines.length; j++) {
            if (closeRe.test(lines[j])) {
                end = j;
                break;
            }
        }

        if (end >= 0 && langs.includes(name)) {
            inKeptLang = true;
            keptLangs.push(name);
            out.push(line);
            continue;
        }

        if (end >= 0) {
            inKeptLang = false;
            droppedLangs.push(name);
            i = end;
            continue;
        }
    }

    const keyMatch = line.match(keyRe);

    if (inKeptLang && keyMatch && dropNs.length) {
        const indent = keyMatch[1];
        const key = keyMatch[2];
        const ns = key.split(".")[0];

        /* Only whole language blocks end with the closing brace of the
         * language, so a key line is always self contained. */
        if (dropNs.includes(ns) || dropNs.includes("*")) {
            if (isReferenced(key)) {
                vetoed.push(key);
            } else {
                droppedKeys.set(ns, (droppedKeys.get(ns) || 0) + 1);
                continue;
            }
        }

        keptKeys.add(key);
        out.push(line);
        continue;
    }

    /* The last line of a language block ends the block. */
    if (inKeptLang && /^\s*\},?\s*$/.test(line))
        inKeptLang = false;

    out.push(line);
}

if (!keptLangs.length)
    fail(`no requested language (${langs.join(",")}) found in ${inputPath}`);

for (const ns of dropNs)
    if (ns !== "*" && !droppedKeys.has(ns))
        console.log(`[i18n] note: --drop-ns "${ns}" matched no key`);

const droppedTotal = [...droppedKeys.values()].reduce((a, b) => a + b, 0);
const detail = [...droppedKeys.entries()]
    .sort((a, b) => b[1] - a[1])
    .map(([ns, count]) => `${ns}=${count}`)
    .join(" ");

try {
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    fs.writeFileSync(outputPath, out.join("\n"), "utf8");
} catch (error) {
    fail(`write failed for ${outputPath}: ${error.message}`);
}

const report = [`kept ${keptLangs.join(",")}`];

if (droppedLangs.length)
    report.push(`dropped ${droppedLangs.join(",")}`);
if (droppedTotal)
    report.push(`${droppedTotal} unused keys dropped (${detail})`);
if (vetoed.length) {
    const shown = vetoed.slice(0, 4).join(" ");

    report.push(`${vetoed.length} kept by the assets (${shown}` +
        `${vetoed.length > 4 ? ", ..." : ""})`);
}

console.log(`[i18n] ${path.basename(inputPath)}: ${report.join(", ")}`);
