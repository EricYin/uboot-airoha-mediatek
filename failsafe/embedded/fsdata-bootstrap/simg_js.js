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
            return;
        }

        var html = '';
        html += '<div class="sysinfo-line">' + t("simg.info.name") + ' ' + flash.name + '</div>';
        html += '<div class="sysinfo-line">' + t("simg.info.size") + ' ' + human(flash.size) + '</div>';
        html += '<div class="sysinfo-line">' + t("simg.info.erasesize") + ' ' + human(flash.erasesize) + '</div>';
        html += '<div class="sysinfo-line">' + t("simg.info.writesize") + ' ' + human(flash.writesize) + '</div>';
        el.innerHTML = html;
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

        var writeButton = document.getElementById("simg_btn_write");
        if (writeButton) writeButton.addEventListener("click", writeSimg);

        var fileInput = document.getElementById("simg_file");
        if (fileInput) fileInput.addEventListener("change", updateFileInfo);
    };
})();
