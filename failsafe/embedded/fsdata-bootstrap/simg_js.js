/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use this file, copy, modify or distribute this file except in
 * compliance with the license agreement.
 *
 * SIMG (single image / ROM dump) frontend logic
 *
 * The image is never uploaded as a whole: it is sliced into erase-block
 * aligned chunks and streamed to /simg/write one chunk at a time, so a
 * full flash dump can be restored without staging it in RAM.
 */

(function () {
    "use strict";

    var simgInfo = null;
    var simgBusy = false;

    function setStatus(message, isError) {
        var el = document.getElementById("simg_status");
        if (!el) return;
        el.textContent = message || "";
        el.className = "settings-status" + (isError ? " red" : "");
    }

    function setProgress(percent) {
        var el = document.getElementById("simg_bar");
        if (!el) return;
        if (percent === null || percent === undefined) {
            el.style.display = "none";
            return;
        }
        el.style.display = "block";
        el.style.setProperty("--percent", Math.max(0, Math.min(100, parseInt(percent || 0))));
    }

    function human(bytes) {
        return typeof bytesToHuman === "function" ? bytesToHuman(bytes) : String(bytes) + " B";
    }

    function toHex(value) {
        return "0x" + Number(value).toString(16);
    }

    function currentFlash() {
        if (!simgInfo || !simgInfo.targets || !simgInfo.targets.length) return null;
        return simgInfo.targets[0];
    }

    function chunkSizeFor(flash) {
        var maxChunk = (simgInfo && simgInfo.max_chunk) || (4 * 1024 * 1024);
        var eraseSize = (flash && flash.erasesize) || 1;
        var blocks = Math.floor(maxChunk / eraseSize);
        return (blocks > 0 ? blocks : 1) * eraseSize;
    }

    function renderInfo() {
        var el = document.getElementById("simg_info");
        if (!el) return;

        var flash = currentFlash();
        if (!flash) {
            el.innerHTML = '<div class="sysinfo-line">' + t("simg.info.none") + '</div>';
            updateBadSection();
            return;
        }

        var html = '';
        html += '<div class="sysinfo-line">' + t("simg.info.name") + ' ' + flash.name + '</div>';
        html += '<div class="sysinfo-line">' + t("simg.info.size") + ' ' + human(flash.size) + '</div>';
        html += '<div class="sysinfo-line">' + t("simg.info.erasesize") + ' ' + human(flash.erasesize) + '</div>';
        html += '<div class="sysinfo-line">' + t("simg.info.writesize") + ' ' + human(flash.writesize) + '</div>';
        el.innerHTML = html;
        updateBadSection();
        updateFileInfo();
    }

    function updateFileInfo() {
        var el = document.getElementById("simg_file_info");
        if (!el) return;

        var fileInput = document.getElementById("simg_file");
        var file = fileInput && fileInput.files && fileInput.files[0];
        if (!file) {
            el.innerHTML = '<div class="sysinfo-line">' + t("simg.detected.none") + '</div>';
            return;
        }

        var flash = currentFlash();
        var text = file.name + " (" + human(file.size) + ")";
        if (flash && file.size !== flash.size) {
            text += " — " + t("simg.detected.mismatch").replace("$1", human(flash.size));
        }
        el.innerHTML = '<div class="sysinfo-line">' + text + '</div>';
    }

    function fetchInfo() {
        var el = document.getElementById("simg_info");
        if (el) el.innerHTML = '<div class="sysinfo-line">' + t("simg.loading") + '</div>';
        ajax({
            url: "/simg/info",
            done: function (resp) {
                try {
                    simgInfo = JSON.parse(resp);
                } catch (e) {
                    setStatus(t("simg.error.parse"), true);
                    return;
                }
                renderInfo();
            }
        });
    }

    /* ── Bad block map ───────────────────────────────────────────── */

    /* Cell size in pixels: a 128 MiB chip has 1024 blocks and a 512 MiB one
     * 4096, so the map is scaled down as the chip grows to stay readable.
     */
    var BAD_CELL_BIG = 16;
    var BAD_CELL_MID = 10;
    var BAD_CELL_SMALL = 6;

    /* Bad blocks listed below the map, as "#index offset" chips. */
    var BAD_LIST_MAX = 24;

    function badCellSize(blocks) {
        if (blocks <= 256) return BAD_CELL_BIG;
        if (blocks <= 1024) return BAD_CELL_MID;
        return BAD_CELL_SMALL;
    }

    function badCellTitle(index, eraseSize) {
        return t("simg.bad.cell")
            .replace("$1", index)
            .replace("$2", toHex(index * eraseSize));
    }

    /* Drop everything the last scan drew, so the next one starts clean. */
    function resetBadResult() {
        var result = document.getElementById("simg_bad_result");
        var map = document.getElementById("simg_bad_map");
        var info = document.getElementById("simg_bad_info");
        var list = document.getElementById("simg_bad_list");

        if (result) result.style.display = "none";
        if (map) map.innerHTML = "";
        if (info) info.innerHTML = "";
        if (list) list.innerHTML = "";
    }

    /*
     * The map costs a scan of every block of the chip, so the section is
     * offered (and the button with it) only on a device that can have bad
     * blocks at all - NOR has none - and nothing is drawn until the user
     * asks for it.
     */
    function updateBadSection() {
        var section = document.getElementById("simg_bad_section");
        var flash = currentFlash();

        if (!section) return;

        if (!flash || !flash.bb) {
            section.style.display = "none";
            resetBadResult();
            return;
        }

        section.style.display = "";
    }

    /*
     * Draw the bad block map of the chip: one cell per erase block, in
     * order, so a cluster of bad blocks is visible as a cluster of red
     * cells.
     */
    function renderBadBlocks(data) {
        var section = document.getElementById("simg_bad_section");
        var result = document.getElementById("simg_bad_result");
        if (!section) return;

        if (!data || !data.ok || !data.bb || !data.blocks) {
            section.style.display = "none";
            resetBadResult();
            return;
        }

        section.style.display = "";
        if (result) result.style.display = "";

        var eraseSize = data.erasesize || 0;
        var bad = data.bad || [];
        var percent = data.bad_count ?
            (data.bad_count / data.blocks * 100).toFixed(2) : "0";

        var info = document.getElementById("simg_bad_info");
        if (info) {
            info.innerHTML =
                '<div class="sysinfo-line">' + t("simg.bad.blocks") + ' ' +
                data.blocks + ' × ' + human(eraseSize) + '</div>' +
                '<div class="sysinfo-line">' + t("simg.bad.count") + ' ' +
                data.bad_count + ' (' + percent + '%)</div>';
        }

        var map = document.getElementById("simg_bad_map");
        if (map) {
            map.style.setProperty("--bbcell", badCellSize(data.blocks) + "px");

            var isBad = {};
            var i;
            for (i = 0; i < bad.length; i++) isBad[bad[i]] = true;

            var cells = [];
            for (i = 0; i < data.blocks; i++) {
                if (isBad[i]) {
                    cells.push('<i class="bbcell bbcell-bad" title="' +
                        badCellTitle(i, eraseSize) + '"></i>');
                } else {
                    cells.push('<i class="bbcell"></i>');
                }
            }
            map.innerHTML = cells.join("");
        }

        var list = document.getElementById("simg_bad_list");
        if (list) {
            var chips = [];
            var shown = Math.min(bad.length, BAD_LIST_MAX);
            var j;

            for (j = 0; j < shown; j++) {
                chips.push('<span>#' + bad[j] + ' ' +
                    toHex(bad[j] * eraseSize) + '</span>');
            }

            if (data.truncated || bad.length > shown) {
                chips.push('<span class="bbmap-more">' +
                    t("simg.bad.truncated") + '</span>');
            }

            list.innerHTML = chips.join("");
        }
    }

    /* Scan button state: the scan reads every block, so it takes a moment
     * and must not be started twice by an impatient second click.
     */
    function badScanBusy(busy) {
        var button = document.getElementById("simg_btn_bad");
        if (!button) return;

        button.disabled = !!busy;
        button.textContent = busy ? t("simg.bad.scanning") : t("simg.bad.scan");
    }

    function fetchBadBlocks() {
        var button = document.getElementById("simg_btn_bad");

        if (button && button.disabled) return;
        badScanBusy(true);

        ajax({
            url: "/simg/badblocks",
            done: function (resp) {
                var data = null;

                try {
                    data = JSON.parse(resp);
                } catch (e) {
                    data = null;
                }

                renderBadBlocks(data);
                badScanBusy(false);
            },
            /* No device to map, or an answer we cannot read: the map stays
             * hidden and the device info above says what is wrong. */
            fail: function () {
                renderBadBlocks(null);
                badScanBusy(false);
            }
        });
    }

    /* Upload one chunk; resolves with the server JSON payload. */
    function sendChunk(blob, start, end, onProgress) {
        return new Promise(function (resolve, reject) {
            var formData = new FormData();
            formData.append("start", toHex(start));
            formData.append("end", toHex(end));
            formData.append("data", blob, "simg_chunk.bin");

            var xhr = new XMLHttpRequest();
            xhr.upload.addEventListener("progress", function (evt) {
                if (evt && evt.lengthComputable) onProgress(start + evt.loaded);
            });
            xhr.addEventListener("readystatechange", function () {
                if (xhr.readyState !== 4) return;
                var payload = null;
                try {
                    payload = JSON.parse(xhr.responseText);
                } catch (e) {
                    payload = null;
                }
                if (xhr.status === 200 && payload && payload.ok) {
                    resolve(payload);
                    return;
                }
                reject(new Error((payload && payload.error) ? payload.error : "http_" + xhr.status));
            });
            xhr.open("POST", "/simg/write");
            xhr.send(formData);
        });
    }

    async function writeSimg() {
        if (simgBusy) return;

        var fileInput = document.getElementById("simg_file");
        var file = fileInput && fileInput.files && fileInput.files[0];
        if (!file || !file.size) {
            setStatus(t("simg.error.no_file"), true);
            return;
        }

        var flash = currentFlash();
        if (!flash) {
            setStatus(t("simg.error.no_flash"), true);
            return;
        }

        if (file.size > flash.size) {
            setStatus(t("simg.error.too_big").replace("$1", human(flash.size)), true);
            return;
        }

        if (file.size !== flash.size) {
            var mismatchMsg = t("simg.confirm.size_mismatch")
                .replace("$1", human(file.size))
                .replace("$2", human(flash.size));
            if (!confirm(mismatchMsg)) return;
        }

        if (!confirm(t("simg.confirm.write").replace("$1", flash.name))) return;

        simgBusy = true;
        setProgress(0);
        setStatus(t("simg.status.writing").replace("$1", human(file.size)));

        try {
            var chunkSize = chunkSizeFor(flash);
            var offset = 0;
            var written = 0;
            var skipped = 0;

            while (offset < file.size) {
                var next = Math.min(offset + chunkSize, file.size);
                var payload = await sendChunk(file.slice(offset, next), offset, next, function (doneBytes) {
                    setProgress(doneBytes / file.size * 100);
                    setStatus(t("simg.status.uploading") + " " + human(doneBytes) + " / " + human(file.size));
                });
                written += payload.written || 0;
                skipped += payload.skipped || 0;
                offset = next;
                setProgress(offset / file.size * 100);
                setStatus(t("simg.status.chunk").replace("$1", toHex(offset)).replace("$2", human(file.size)));
            }

            setProgress(100);
            setStatus(t("simg.status.done").replace("$1", human(written)) +
                (skipped ? " " + t("simg.status.skipped").replace("$1", human(skipped)) : ""));
        } catch (error) {
            setStatus(t("simg.error.write_failed") + " " + (error && error.message ? error.message : String(error)), true);
        } finally {
            simgBusy = false;
        }
    }

    window.simgInit = function () {
        fetchInfo();
        updateFileInfo();

        var refreshButton = document.getElementById("simg_btn_refresh");
        if (refreshButton) refreshButton.addEventListener("click", fetchInfo);

        /* The bad block map is drawn on demand only: reading every block
         * of the chip is not something to do on every page load. */
        var badButton = document.getElementById("simg_btn_bad");
        if (badButton) badButton.addEventListener("click", fetchBadBlocks);

        var writeButton = document.getElementById("simg_btn_write");
        if (writeButton) writeButton.addEventListener("click", writeSimg);

        var fileInput = document.getElementById("simg_file");
        if (fileInput) fileInput.addEventListener("change", updateFileInfo);
    };
})();
