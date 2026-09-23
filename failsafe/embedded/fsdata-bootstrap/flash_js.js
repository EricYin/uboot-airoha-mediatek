/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe flash management frontend
 *
 * One page covering everything the former backup / flash editor pages
 * did: chip identification, backup download, hex editing, restore and
 * erase of raw storage targets.
 */

(function () {
    "use strict";

    /* ── Constants ── */
    var FLASH_PAGE_SIZE = 512;                /* bytes per hex page (32 rows) */
    var FLASH_READ_CHUNK = 256 * 1024;        /* must match backend FLASH_READ_CHUNK */
    var FLASH_RESTORE_CHUNK = 4 * 1024 * 1024;
    var FLASH_MAX_SELECTION = 256;            /* max bytes selectable at once */
    var HEX_ROW = 16;                         /* bytes per hex row */

    /* ── Hex editor state ── */
    var hexBytes = [];
    var hexModified = new Set();
    var selectedByte = -1;
    var selectionStart = -1;                  /* anchor of the range selection */
    var selectionEnd = -1;                    /* other end of the range selection */
    var isDragging = false;
    var dragAnchor = -1;
    var currentPage = 0;
    var readBase = 0;                         /* absolute address of the read range */
    var totalPages = 0;
    var restoreAlert = "";

    /* ── Small helpers ── */
    function $(id) {
        return document.getElementById(id);
    }

    function human(value) {
        return typeof bytesToHuman === "function" ? bytesToHuman(value) : String(value) + " B";
    }

    function toHex(value) {
        return "0x" + Number(value).toString(16);
    }

    function padHexByte(value) {
        var text = Number(value).toString(16).toUpperCase();
        return text.length < 2 ? "0" + text : text;
    }

    function errText(error) {
        return error && error.message ? error.message : String(error);
    }

    /* ── Status / progress ── */
    function setStatus(message, isError, isBusy) {
        var box = $("flash_status");
        var text = $("flash_status_text");
        var spinner = $("flash_spinner");
        if (!box) return;
        box.style.display = message ? "flex" : "none";
        box.className = "flash-status" + (isError ? " red" : "");
        if (text) text.textContent = message || "";
        if (spinner) spinner.style.display = isBusy ? "block" : "none";
    }

    function setProgress(barId, percent) {
        var bar = $(barId);
        if (!bar) return;
        if (percent === null || percent === undefined) {
            bar.style.display = "none";
            return;
        }
        bar.style.display = "block";
        bar.style.setProperty("--percent", Math.max(0, Math.min(100, parseInt(percent || 0))));
    }

    /* ── Target selection ── */
    function currentOption() {
        var select = $("flash_target");
        if (!select || select.selectedIndex < 0) return null;
        return select.options[select.selectedIndex] || null;
    }

    function currentTargetValue() {
        var select = $("flash_target");
        return select && select.value ? String(select.value) : "";
    }

    function optionSize(option) {
        if (!option || !option.dataset || !option.dataset.size) return null;
        var size = parseInt(option.dataset.size, 10);
        return isFinite(size) && size > 0 ? size : null;
    }

    /* Operation mode: the whole target or a user typed custom range. */
    function operationMode() {
        var select = $("flash_mode");
        return select ? select.value : "part";
    }

    function selectMode(mode) {
        var select = $("flash_mode");
        if (select) select.value = mode;
    }

    /* Range explicitly typed by the user; null when empty / inverted. */
    function explicitRange() {
        var startInput = $("flash_start");
        var endInput = $("flash_end");
        var start = startInput ? parseUserLen(startInput.value) : null;
        var end = endInput ? parseUserLen(endInput.value) : null;
        if (start === null || end === null || end <= start) return null;
        return { start: start, end: end };
    }

    /* Range the current operation works on, derived from the mode. */
    function effectiveRange() {
        if (operationMode() === "range") return explicitRange();

        var size = optionSize(currentOption());
        return size === null ? null : { start: 0, end: size };
    }

    /* Pre-fill the range fields with the whole target as a starting point. */
    function autoFillRange() {
        var size = optionSize(currentOption());
        var startInput = $("flash_start");
        var endInput = $("flash_end");
        if (size === null || !startInput || !endInput) return;
        startInput.value = "0x0";
        endInput.value = toHex(size);
    }

    function updateRangeHint() {
        var hint = $("flash_range_hint");
        if (!hint) return;

        var range = effectiveRange();
        if (!range) {
            hint.textContent = t("flash.range.hint");
            return;
        }

        if (operationMode() !== "range") {
            hint.textContent = t("flash.range.whole")
                .replace("$1", human(range.end - range.start));
            return;
        }

        hint.textContent = t("flash.range.summary")
            .replace("$1", human(range.start))
            .replace("$2", human(range.end))
            .replace("$3", human(range.end - range.start));
    }

    /* The Start / End fields are only shown in custom range mode. */
    function updateModeUi() {
        var row = $("flash_range_row");
        if (row) row.style.display = operationMode() === "range" ? "" : "none";
        updateRangeHint();
    }

    function selectTargetByValue(value) {
        var select = $("flash_target");
        if (!select) return false;
        for (var i = 0; i < select.options.length; i++) {
            if (select.options[i].value === value) {
                select.selectedIndex = i;
                return true;
            }
        }
        return false;
    }

    /*
     * "[MMC] boot0 (512.00 KiB)" - the raw regions of the device (the boot
     * partitions and the read-only RPMB), named by /flash/info.
     */
    function regionLabel(region) {
        var name = region.name || "";
        var label = t("flash.target." + name, name);

        return "[MMC] " + label +
            (region.size ? " (" + human(region.size) + ")" : "") +
            (region.ro ? " (" + t("flash.target.readonly", "read only") + ")" : "");
    }

    function refreshTargetI18n() {
        var select = $("flash_target");
        if (!select) return;
        for (var i = 0; i < select.options.length; i++) {
            var option = select.options[i];
            if (!option || !option.dataset) continue;
            if (option.dataset.i18nKey) {
                option.textContent = t(option.dataset.i18nKey);
                continue;
            }
            if (option.dataset.kind === "mmc-region") {
                option.textContent = regionLabel({
                    name: option.dataset.regionName,
                    size: option.dataset.size ? parseInt(option.dataset.size, 10) : 0,
                    ro: option.dataset.ro === "1"
                });
            } else if (option.dataset.kind === "mtd-full") {
                var name = option.dataset.mtdName || "";
                option.textContent = "[MTD] " + t("flash.target.full_disk") +
                    (name ? " (" + name + ")" : "") +
                    (option.dataset.size ? " (" + human(parseInt(option.dataset.size, 10)) + ")" : "");
            } else if (option.dataset.kind === "mtd-oob") {
                var oobName = option.dataset.mtdName || "";
                option.textContent = "[MTD] " + t("flash.target.full_disk") +
                    (oobName ? " (" + oobName + ")" : "") + " (OOB" +
                    (option.dataset.size ? " " + human(parseInt(option.dataset.size, 10)) : "") + ")";
            }
        }
    }

    /* Device identification, rendered as one plain line (no card). */
    function renderDeviceInfo() {
        var box = $("flash_device_info");
        if (!box) return;

        var info = APP_STATE.flashinfo;
        var parts = [];

        if (info && info.mmc && info.mmc.present) {
            var mmcName = [info.mmc.vendor || "", info.mmc.product || ""].join(" ").trim();
            parts.push(t("flash.device.mmc") + " " + (mmcName || "-") +
                (info.mmc.size ? " (" + human(info.mmc.size) + ")" : ""));
        } else {
            parts.push(t("flash.device.mmc") + " " + t("flash.device.not_present"));
        }

        if (info && info.mtd && info.mtd.present) {
            parts.push(t("flash.device.mtd") + " " + (info.mtd.model || "-"));
        } else {
            parts.push(t("flash.device.mtd") + " " + t("flash.device.not_present"));
        }

        /* NAND raw (OOB) dumps need enough free RAM for the whole chip. */
        if (info && info.mtd && info.mtd.nand_raw_size > 0) {
            var ramOk = info.mtd.ram_available >= info.mtd.nand_raw_size;
            parts.push(t("flash.device.nand_raw") + " " +
                human(info.mtd.nand_page_size) + " + " +
                human(info.mtd.nand_oob_size) + " OOB = " +
                human(info.mtd.nand_raw_size) +
                (ramOk ? "" : " (" + t("flash.device.nand_raw_no_ram") + ")"));
        }

        box.textContent = parts.join(" | ");
    }


    /* ── Flash layouts of the device tree (optional feature) ──
     *
     * GET /flash/layouts lists the layouts the firmware was built with (see
     * CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT and failsafe/modules/flash.c):
     * picking one replaces the target list with the partitions of that
     * layout, which are raw flash ranges instead of partition table entries.
     * /flash/info says whether the feature is there at all, so the picker
     * stays hidden on firmware without it.
     */
    function currentLayout() {
        var select = $("flash_layout");
        return select && select.value ? String(select.value) : "";
    }

    function currentLayoutParts() {
        var layouts = (APP_STATE.flashlayouts && APP_STATE.flashlayouts.layouts) || [];
        var name = currentLayout();
        for (var i = 0; i < layouts.length; i++) {
            if (layouts[i] && layouts[i].name === name) return layouts[i].parts || [];
        }
        return null;
    }

    /* Capacity of the raw device a layout is expressed in: the master MTD
     * chip when the board has one, the MMC user area otherwise - the same
     * choice the firmware makes (see flash_open_layout_target()). */
    function deviceCapacity() {
        var info = APP_STATE.flashinfo || {};
        var mtd = info.mtd || {};
        var mmc = info.mmc || {};
        var master = null;

        (mtd.parts || []).forEach(function (part) {
            if (part && part.master && !master) master = part;
        });
        if (master && master.size) return Number(master.size);

        return mmc.size ? Number(mmc.size) : 0;
    }

    function updateLayoutHint() {
        var hint = $("flash_layout_hint");
        var active = currentLayout();

        if (!hint) return;
        hint.style.display = active ? "" : "none";
    }

    /* Fill the picker with the layouts of the firmware, or keep it out of the
     * way when there is nothing to choose from. */
    function populateLayouts() {
        var row = $("flash_layout_row");
        var select = $("flash_layout");
        var info = APP_STATE.flashinfo || {};
        var layouts = (APP_STATE.flashlayouts && APP_STATE.flashlayouts.layouts) || [];
        var keep = currentLayout();

        if (!select || !row) return;

        if (info.layout !== true || !layouts.length) {
            row.style.display = "none";
            select.options.length = 0;
            updateLayoutHint();
            return;
        }

        select.options.length = 0;

        var current = document.createElement("option");
        current.value = "";
        current.textContent = t("flash.layout.current");
        select.appendChild(current);

        layouts.forEach(function (layout) {
            if (!layout || !layout.name) return;
            var option = document.createElement("option");
            option.value = layout.name;
            option.textContent = layout.name;
            select.appendChild(option);
        });

        if (keep) select.value = keep;
        if (!select.value) select.value = "";
        row.style.display = "";
        updateLayoutHint();
    }

    function bindLayoutPicker() {
        var select = $("flash_layout");
        if (!select || select.dataset.bound === "1") return;
        select.dataset.bound = "1";
        select.addEventListener("change", function () {
            populateTargets();
            updateLayoutHint();
        });
    }

    /* The targets of the picked layout: raw flash ranges, so their size comes
     * from the layout (a size of 0 meaning "to the end of the device"). */
    function appendLayoutTargets(select, parts) {
        var capacity = deviceCapacity();

        parts.forEach(function (part) {
            var offset, size, option;

            if (!part || !part.name) return;
            offset = parseUserLen(part.offset);
            size = parseUserLen(part.size);
            if (offset === null || size === null) return;
            if (!size) size = capacity > offset ? capacity - offset : 0;

            option = document.createElement("option");
            option.value = part.name;
            option.dataset.kind = "layout-part";
            option.dataset.size = String(size);
            option.dataset.offset = String(offset);
            option.textContent = part.name + " @ " + toHex(offset) +
                (size ? " (" + human(size) + ")" : "");
            select.appendChild(option);
        });
    }

    /* GET /flash/layouts, then (re)build the picker and the target list. */
    function loadLayouts() {
        var info = APP_STATE.flashinfo || {};
        var done = function () {
            populateLayouts();
            bindLayoutPicker();
            renderDeviceInfo();
            populateTargets();
        };

        if (info.layout !== true) {
            APP_STATE.flashlayouts = null;
            done();
            return;
        }

        ajax({
            url: "/flash/layouts",
            done: function (responseText) {
                try {
                    APP_STATE.flashlayouts = JSON.parse(responseText);
                } catch (error) {
                    APP_STATE.flashlayouts = null;
                }
                done();
            },
            fail: function () {
                APP_STATE.flashlayouts = null;
                done();
            }
        });
    }

    function populateTargets() {
        var select = $("flash_target");
        if (!select) return;

        var info = APP_STATE.flashinfo || {};
        var mtd = info.mtd || {};
        var mmc = info.mmc || {};
        var nandRawSize = mtd.nand_raw_size || 0;
        var ramAvailable = mtd.ram_available || 0;
        var masterPartitions = [];
        var hasMasterPartitions = mtd.type === 3 || mtd.type === 4 || mtd.type === 8;

        select.options.length = 0;

        /* A device-tree layout replaces the partition table view. */
        var layoutParts = currentLayoutParts();

        if (layoutParts) {
            appendLayoutTargets(select, layoutParts);
            if (select.options.length > 1) select.selectedIndex = 1;
            refreshTargetI18n();
            autoFillRange();
            updateModeUi();
            updateLayoutHint();
            return;
        }

        var placeholder = document.createElement("option");
        placeholder.value = "";
        placeholder.dataset.i18nKey = "flash.target.placeholder";
        placeholder.textContent = t("flash.target.placeholder");
        select.appendChild(placeholder);

        if (mmc.present) {
            var rawOption = document.createElement("option");
            rawOption.value = "mmc:raw";
            rawOption.dataset.kind = "mmc-raw";
            rawOption.dataset.size = mmc.size ? String(mmc.size) : "";
            rawOption.textContent = "[MMC] raw";
            select.appendChild(rawOption);

            (mmc.parts || []).forEach(function (part) {
                if (!part || !part.name) return;
                var option = document.createElement("option");
                option.value = "mmc:" + part.name;
                option.dataset.kind = "mmc-part";
                option.dataset.size = part.size ? String(part.size) : "";
                option.textContent = "[MMC] " + part.name +
                    (part.size ? " (" + human(part.size) + ")" : "");
                select.appendChild(option);
            });

            /* Raw regions that are not partitions: boot0 / boot1 (read
             * and write) and the RPMB (read only). */
            (mmc.regions || []).forEach(function (region) {
                if (!region || !region.name) return;
                var option = document.createElement("option");
                option.value = "mmc:" + region.name;
                option.dataset.kind = "mmc-region";
                option.dataset.regionName = region.name;
                option.dataset.size = region.size ? String(region.size) : "";
                option.dataset.ro = region.ro ? "1" : "0";
                option.textContent = regionLabel(region);
                select.appendChild(option);
            });
        }

        if (mtd.present && mtd.parts && mtd.parts.length) {
            if (hasMasterPartitions) {
                mtd.parts.forEach(function (part) {
                    if (part && part.name && part.master) masterPartitions.push(part);
                });
            }

            /* Raw chip dumps, offered once per master device. */
            masterPartitions.forEach(function (part) {
                var fullDisk = document.createElement("option");
                fullDisk.value = "mtd:" + part.name;
                fullDisk.dataset.kind = "mtd-full";
                fullDisk.dataset.mtdName = part.name;
                fullDisk.dataset.size = part.size ? String(part.size) : "";
                select.appendChild(fullDisk);

                /* OOB raw dump: physical NAND only, skip the NMBM layer. */
                if (nandRawSize > 0 && ramAvailable > 0 &&
                    nandRawSize <= ramAvailable &&
                    !String(part.name).startsWith("nmbm")) {
                    var oob = document.createElement("option");
                    oob.value = "mtd:" + part.name;
                    oob.dataset.kind = "mtd-oob";
                    oob.dataset.mtdName = part.name;
                    oob.dataset.size = String(nandRawSize);
                    oob.dataset.raw = "1";
                    select.appendChild(oob);
                }
            });

            mtd.parts.forEach(function (part) {
                if (!part || !part.name) return;
                if (hasMasterPartitions && part.master) return;
                var option = document.createElement("option");
                option.value = "mtd:" + part.name;
                option.dataset.kind = "mtd-part";
                option.dataset.size = part.size ? String(part.size) : "";
                option.textContent = "[MTD] " + part.name +
                    (part.size ? " (" + human(part.size) + ")" : "");
                select.appendChild(option);
            });
        }

        if (select.options.length > 1) select.selectedIndex = 1;

        refreshTargetI18n();
        autoFillRange();
        updateModeUi();
    }

    function refreshInfo() {
        var box = $("flash_device_info");
        if (box) box.textContent = t("flash.loading");

        ajax({
            url: "/flash/info",
            done: function (responseText) {
                var parsed;
                try {
                    parsed = JSON.parse(responseText);
                } catch (error) {
                    setStatus(t("flash.error.parse"), true, false);
                    return;
                }
                APP_STATE.flashinfo = parsed;
                loadLayouts();
            },
            fail: function () {
                setStatus(t("flash.error.parse"), true, false);
            }
        });
    }

    /* ── Backup download ── */
    async function startBackup() {
        var target = currentTargetValue();
        if (!target) {
            setStatus(t("flash.error.no_target"), true, false);
            return;
        }

        var mode = operationMode();
        var option = currentOption();
        var formData = new FormData();

        formData.append("mode", mode);
        formData.append("storage", "auto");
        formData.append("target", target);
        formData.append("layout", currentLayout());
        if (option && option.dataset && option.dataset.raw === "1")
            formData.append("raw", "1");

        if (mode === "range") {
            var range = explicitRange();
            if (!range) {
                setStatus(t("flash.error.bad_range"), true, false);
                return;
            }
            formData.append("start", toHex(range.start));
            formData.append("end", toHex(range.end));
        }

        setProgress("flash_backup_bar", 0);
        setStatus(t("flash.status.backup"), false, true);

        try {
            var response = await fetch("/flash/backup", { method: "POST", body: formData });
            if (!response.ok) {
                setStatus(t("flash.status.http") + " " + response.status, true, false);
                return;
            }

            var contentLength = response.headers.get("Content-Length");
            var expected = contentLength ? parseInt(contentLength, 10) : 0;
            var downloadName = parseFilenameFromDisposition(response.headers.get("Content-Disposition"));
            if (!downloadName) downloadName = "backup.bin";

            await ensureSysInfoLoaded();
            downloadName = makeBackupDownloadName(downloadName);

            var reader = response.body.getReader();
            var downloaded = 0;
            var pending = [];
            var writable = null;

            if (window.showSaveFilePicker) {
                var handle = await window.showSaveFilePicker({
                    suggestedName: downloadName,
                    types: [{ description: "Binary", accept: { "application/octet-stream": [".bin"] } }]
                });
                writable = await handle.createWritable();
            }

            while (true) {
                var chunk = await reader.read();
                if (chunk.done) break;
                if (writable) await writable.write(chunk.value);
                else pending.push(chunk.value);

                downloaded += chunk.value.length;
                setProgress("flash_backup_bar", expected ? downloaded / expected * 100 : 0);
                setStatus(t("flash.status.backup") + " " + human(downloaded) +
                    (expected ? " / " + human(expected) : ""), false, true);
            }

            if (writable) {
                await writable.close();
            } else {
                var blob = new Blob(pending, { type: "application/octet-stream" });
                var link = document.createElement("a");
                link.href = URL.createObjectURL(blob);
                link.download = downloadName;
                document.body.appendChild(link);
                link.click();
                document.body.removeChild(link);
            }

            setProgress("flash_backup_bar", 100);
            setStatus(t("flash.status.done") + " " + downloadName, false, false);
        } catch (error) {
            setStatus(t("flash.status.error") + " " + errText(error), true, false);
        }
    }

    /* ── Hex grid ── */

    /* Sorted selection range { start, end, count }, or null when empty. */
    function getSelectionRange() {
        if (selectionStart < 0 || selectionEnd < 0) return null;
        var low = Math.min(selectionStart, selectionEnd);
        var high = Math.max(selectionStart, selectionEnd);
        return { start: low, end: high, count: high - low + 1 };
    }

    /* Set both ends, clamping the selection to FLASH_MAX_SELECTION bytes. */
    function setSelectionRange(anchor, cursor) {
        var maxEnd = anchor + FLASH_MAX_SELECTION - 1;
        var minEnd = anchor - FLASH_MAX_SELECTION + 1;
        var clamped = cursor < anchor ? Math.max(cursor, minEnd) : Math.min(cursor, maxEnd);
        selectionStart = anchor;
        selectionEnd = clamped;
        selectedByte = cursor;
        renderHexGrid();
        var input = $("flash_hex_input");
        if (input) { input.value = ""; input.focus(); }
    }

    function hexStrToBytes(hexStr) {
        var matches = (hexStr || "").match(/[0-9a-fA-F]{2}/g);
        if (!matches) return [];
        var out = [];
        for (var i = 0; i < matches.length; i++) out.push(parseInt(matches[i], 16));
        return out;
    }

    function ensureHexSelected() {
        if (hexBytes.length === 0) return false;
        if (selectedByte < 0 || selectedByte >= hexBytes.length) selectedByte = 0;
        return true;
    }

    /* Render the byte grid, current page only. */
    function renderHexGrid() {
        var grid = $("flash_hex_grid");
        if (!grid) return;

        var range = getSelectionRange();
        var pageStart = currentPage * FLASH_PAGE_SIZE;
        var pageEnd = Math.min(pageStart + FLASH_PAGE_SIZE, hexBytes.length);

        grid.innerHTML = "";
        for (var row = pageStart; row < pageEnd; row += HEX_ROW) {
            var rowDiv = document.createElement("div");
            rowDiv.className = "hex-row";
            for (var col = 0; col < HEX_ROW && row + col < pageEnd; col++) {
                var index = row + col;
                var span = document.createElement("span");
                span.className = "hex-byte";
                span.dataset.index = index;
                span.textContent = padHexByte(hexBytes[index]);
                if (range && index >= range.start && index <= range.end) span.classList.add("in-range");
                if (index === selectedByte) span.classList.add("selected");
                if (hexModified.has(index)) span.classList.add("modified");
                rowDiv.appendChild(span);
            }
            grid.appendChild(rowDiv);
        }
    }

    /* Render the offset and ASCII columns, current page only. */
    function renderHexViews() {
        var offsetEl = $("flash_offset");
        var asciiEl = $("flash_ascii");
        if (!offsetEl || !asciiEl) return;

        var pageStart = currentPage * FLASH_PAGE_SIZE;
        var pageEnd = Math.min(pageStart + FLASH_PAGE_SIZE, hexBytes.length);
        var offsets = [];
        var ascii = [];

        for (var row = pageStart; row < pageEnd; row += HEX_ROW) {
            offsets.push("0x" + flashPad(readBase + row, 8));
            for (var col = 0; col < HEX_ROW && row + col < pageEnd; col++) {
                var byte = hexBytes[row + col];
                ascii.push(byte >= 0x20 && byte <= 0x7e ? String.fromCharCode(byte) : ".");
            }
            if (row + HEX_ROW > pageEnd) {
                for (var fill = pageEnd - row; fill < HEX_ROW; fill++) ascii.push(" ");
            }
            ascii.push("\n");
        }

        offsetEl.textContent = offsets.join("\n");
        asciiEl.textContent = ascii.join("").replace(/\n$/, "");
    }

    function flashPad(value, width) {
        var text = Number(value).toString(16).toUpperCase();
        while (text.length < width) text = "0" + text;
        return text;
    }

    /* Select a byte, navigating to its page when needed. */
    function selectByte(index) {
        if (index < 0 || index >= hexBytes.length) return;

        var newPage = Math.floor(index / FLASH_PAGE_SIZE);
        if (newPage !== currentPage) {
            currentPage = newPage;
            updatePageControls();
        }

        selectedByte = index;
        selectionStart = index;
        selectionEnd = index;
        var input = $("flash_hex_input");
        if (input) input.value = "";

        renderHexGrid();
        renderHexViews();
        if (input) input.focus();
        scrollToByte(index);
    }

    function focusHexInput() {
        var input = $("flash_hex_input");
        if (input) input.focus();
    }

    /* Scroll the grid so the row holding byteIndex is visible. */
    function scrollToByte(byteIndex) {
        var grid = $("flash_hex_grid");
        if (!grid || hexBytes.length === 0) return;

        var pageStart = currentPage * FLASH_PAGE_SIZE;
        var rowIndex = Math.floor((byteIndex - pageStart) / HEX_ROW);
        var rows = grid.querySelectorAll(".hex-row");
        if (rows.length > rowIndex && rowIndex >= 0) {
            var row = rows[rowIndex];
            var gridTop = grid.getBoundingClientRect().top;
            var rowTop = row.getBoundingClientRect().top;
            var rowBottom = row.getBoundingClientRect().bottom;
            if (rowTop < gridTop || rowBottom > gridTop + grid.clientHeight)
                row.scrollIntoView({ block: "nearest" });
        }
        syncScroll();
    }

    function syncScroll() {
        var grid = $("flash_hex_grid");
        var offsetEl = $("flash_offset");
        var asciiEl = $("flash_ascii");
        if (!grid || !offsetEl || !asciiEl) return;
        offsetEl.scrollTop = grid.scrollTop;
        asciiEl.scrollTop = grid.scrollTop;
    }

    function jumpToOffset() {
        var jumpInput = $("flash_jump");
        if (!jumpInput || hexBytes.length === 0) return;

        var targetOffset = parseUserLen(jumpInput.value);
        if (targetOffset === null) {
            setStatus(t("flash.error.jump"), true, false);
            return;
        }

        var byteIndex = targetOffset - readBase;
        if (byteIndex < 0 || byteIndex >= hexBytes.length) {
            setStatus(t("flash.error.jump"), true, false);
            return;
        }

        selectByte(byteIndex);
        setStatus("", false, false);
    }

    function goToPage(number) {
        number = Math.max(0, Math.min(number, totalPages - 1));
        if (number === currentPage || !isFinite(number)) return;

        currentPage = number;
        renderHexGrid();
        renderHexViews();
        updatePageControls();

        var grid = $("flash_hex_grid");
        if (grid) grid.scrollTop = 0;
        syncScroll();
    }

    function updatePageControls() {
        var container = $("flash_page_controls");
        var info = $("flash_page_info");
        if (!container || !info) return;

        var show = totalPages > 1;
        container.style.display = show ? "flex" : "none";
        if (show) {
            info.textContent = (currentPage + 1) + " / " + totalPages;
            $("flash_page_prev").disabled = currentPage <= 0;
            $("flash_page_next").disabled = currentPage >= totalPages - 1;
        }
    }

    /* Group the modified bytes into contiguous runs to write. */
    function buildWriteChunks() {
        if (hexModified.size === 0) return [];

        var sorted = Array.from(hexModified).sort(function (a, b) { return a - b; });
        var chunks = [];
        var chunkStart = sorted[0];
        var chunkEnd = sorted[0];

        for (var i = 1; i < sorted.length; i++) {
            if (sorted[i] === chunkEnd + 1) {
                chunkEnd = sorted[i];
            } else {
                chunks.push({ start: chunkStart, end: chunkEnd, count: chunkEnd - chunkStart + 1 });
                chunkStart = sorted[i];
                chunkEnd = sorted[i];
            }
        }
        chunks.push({ start: chunkStart, end: chunkEnd, count: chunkEnd - chunkStart + 1 });
        return chunks;
    }

    function handleHexKey(event) {
        if (hexBytes.length === 0) return;
        ensureHexSelected();

        var key = event.key;
        var previous = selectedByte;
        var consumed = true;

        if (key === "ArrowRight") selectedByte = Math.min(selectedByte + 1, hexBytes.length - 1);
        else if (key === "ArrowLeft") selectedByte = Math.max(selectedByte - 1, 0);
        else if (key === "ArrowDown") selectedByte = Math.min(selectedByte + HEX_ROW, hexBytes.length - 1);
        else if (key === "ArrowUp") selectedByte = Math.max(selectedByte - HEX_ROW, 0);
        else if (key === "Tab") selectedByte = event.shiftKey ? Math.max(selectedByte - 1, 0) : Math.min(selectedByte + 1, hexBytes.length - 1);
        else if (key === "Home") selectedByte = Math.floor(selectedByte / HEX_ROW) * HEX_ROW;
        else if (key === "End") selectedByte = Math.min(Math.floor(selectedByte / HEX_ROW) * HEX_ROW + HEX_ROW - 1, hexBytes.length - 1);
        else if (key === "PageDown") selectedByte = Math.min(selectedByte + 64, hexBytes.length - 1);
        else if (key === "PageUp") selectedByte = Math.max(selectedByte - 64, 0);
        else if (key === "Escape") { consumed = false; $("flash_hex_input").blur(); }
        else consumed = false;

        if (!consumed || selectedByte === previous) return;

        event.preventDefault();
        var input = $("flash_hex_input");
        if (input) input.value = "";

        if (event.shiftKey) {
            if (selectionStart < 0) selectionStart = previous;
            selectionEnd = selectedByte;
        } else {
            selectionStart = selectedByte;
            selectionEnd = selectedByte;
        }

        var newPage = Math.floor(selectedByte / FLASH_PAGE_SIZE);
        if (newPage !== currentPage) {
            currentPage = newPage;
            updatePageControls();
        }

        renderHexGrid();
        renderHexViews();
        focusHexInput();
        scrollToByte(selectedByte);
    }

    /* One byte, or fill the whole selected range with the typed value. */
    function handleHexInput() {
        if (hexBytes.length === 0) return;
        ensureHexSelected();

        var input = $("flash_hex_input");
        if (!input) return;

        var value = input.value.replace(/[^0-9a-fA-F]/g, "").toUpperCase();
        if (value.length > 2) value = value.slice(0, 2);
        input.value = value;
        if (value.length !== 2) return;

        var byteValue = parseInt(value, 16);
        if (isNaN(byteValue)) return;

        var range = getSelectionRange();
        if (range && range.count > 1) {
            for (var i = range.start; i <= range.end; i++) {
                hexBytes[i] = byteValue;
                hexModified.add(i);
            }
            setStatus(t("flash.hex.filled").replace("$1", range.count), false, false);
        } else {
            hexBytes[selectedByte] = byteValue;
            hexModified.add(selectedByte);
        }

        input.value = "";
        if (selectedByte + 1 < hexBytes.length) {
            selectedByte++;
            selectByte(selectedByte);
        } else {
            renderHexGrid();
            renderHexViews();
            focusHexInput();
        }
    }

    function hexByteAtPoint(clientX, clientY) {
        var element = document.elementFromPoint(clientX, clientY);
        if (!element) return -1;
        var target = element.closest(".hex-byte");
        if (!target) return -1;
        return parseInt(target.dataset.index, 10);
    }

    function hexGridMouseDown(event) {
        var index = hexByteAtPoint(event.clientX, event.clientY);
        if (index < 0 || index >= hexBytes.length) return;

        event.preventDefault();
        isDragging = true;
        dragAnchor = index;
        setSelectionRange(index, index);
        document.addEventListener("mousemove", hexGridMouseMove);
        document.addEventListener("mouseup", hexGridMouseUp);
    }

    function hexGridMouseMove(event) {
        if (!isDragging) return;
        var index = hexByteAtPoint(event.clientX, event.clientY);
        if (index < 0 || index >= hexBytes.length) return;
        setSelectionRange(dragAnchor, index);
    }

    function hexGridMouseUp() {
        isDragging = false;
        document.removeEventListener("mousemove", hexGridMouseMove);
        document.removeEventListener("mouseup", hexGridMouseUp);
        focusHexInput();
    }

    function hexGridClick(event) {
        if (isDragging) return;
        var index = hexByteAtPoint(event.clientX, event.clientY);
        if (index >= 0 && index < hexBytes.length) selectByte(index);
    }

    /* ── Backup filename decoding ── */
    function findLastBefore(text, needle, limit) {
        var found = -1;
        var current = text.indexOf(needle);
        while (current !== -1 && current < limit) {
            found = current;
            current = text.indexOf(needle, current + 1);
        }
        return found;
    }

    /* Decode "<storage>_<model>_<target>_0x<start>-0x<end>.bin". */
    function parseBackupFilename(name) {
        if (!name) return null;

        var rangeIndex = name.indexOf("_0x");
        if (rangeIndex < 0) return null;

        var dashIndex = name.indexOf("-0x", rangeIndex);
        if (dashIndex < 0) return null;

        var startMatch = /^0x[0-9a-fA-F]+/.exec(name.slice(rangeIndex + 1, dashIndex));
        var endMatch = /^0x[0-9a-fA-F]+/.exec(name.slice(dashIndex + 1));
        if (!startMatch || !endMatch) return null;

        var start = parseInt(startMatch[0], 16);
        var end = parseInt(endMatch[0], 16);
        if (!isFinite(start) || !isFinite(end) || end <= start) return null;

        var mtdIndex = findLastBefore(name, "_mtd_", rangeIndex);
        var mmcIndex = findLastBefore(name, "_mmc_", rangeIndex);
        var storageIndex = mtdIndex >= 0 && mmcIndex >= 0 ?
            (mtdIndex > mmcIndex ? mtdIndex : mmcIndex) :
            (mtdIndex >= 0 ? mtdIndex : mmcIndex);
        if (storageIndex < 0) return null;

        var storage = storageIndex === mtdIndex ? "mtd" : "mmc";
        var segment = name.slice(storageIndex + 5, rangeIndex);
        if (!segment) return null;

        /* A raw NAND dump carries an extra "_oob" marker before the range. */
        if (segment.length > 4 && segment.slice(-4) === "_oob")
            segment = segment.slice(0, -4);
        if (!segment) return null;

        var parts = segment.split("_");
        var target = parts[parts.length - 1];
        if (!target) return null;

        return { storage: storage, target: target, start: start, end: end };
    }

    function updateRestoreInfo() {
        var box = $("flash_restore_info");
        var input = $("flash_backup");
        var file = input && input.files && input.files.length ? input.files[0] : null;
        if (!box) return;

        if (!file) {
            box.textContent = t("flash.detected.none");
            return;
        }

        var parsed = parseBackupFilename(file.name);
        if (!parsed) {
            box.textContent = t("flash.detected.none") + " (" + human(file.size) + ")";
            return;
        }

        box.textContent = parsed.storage + ":" + parsed.target + " " +
            toHex(parsed.start) + "-" + toHex(parsed.end) + " (" + human(file.size) + ")";

        selectTargetByValue(parsed.storage + ":" + parsed.target);
        var startInput = $("flash_start");
        var endInput = $("flash_end");
        if (startInput) startInput.value = toHex(parsed.start);
        if (endInput) endInput.value = toHex(parsed.end);

        /* Make the detected range visible: switch to custom range mode. */
        selectMode("range");
        updateModeUi();
        renderHexViews();
    }

    /* ── Read / write / erase / restore ── */
    async function doRead() {
        var target = currentTargetValue();
        var range = effectiveRange();
        if (!target) { setStatus(t("flash.error.no_target"), true, false); return; }
        if (!range) { setStatus(t("flash.error.bad_range"), true, false); return; }

        var totalSize = range.end - range.start;
        var totalChunks = Math.ceil(totalSize / FLASH_READ_CHUNK);

        readBase = range.start;
        hexBytes = new Array(totalSize).fill(0);
        hexModified = new Set();
        totalPages = Math.max(1, Math.ceil(totalSize / FLASH_PAGE_SIZE));
        currentPage = 0;
        selectedByte = -1;
        selectionStart = -1;
        selectionEnd = -1;

        if (totalSize > 1024 * 1024 &&
            !confirm(t("flash.confirm.chunk").replace("$1", totalChunks).replace("$2", human(totalSize))))
            return;

        try {
            for (var chunkIndex = 0; chunkIndex < totalChunks; chunkIndex++) {
                setStatus(t("flash.status.reading") + " (" + (chunkIndex + 1) + "/" +
                    totalChunks + ")", false, true);

                var formData = new FormData();
                formData.append("op", "read");
                formData.append("storage", "auto");
                formData.append("target", target);
                formData.append("layout", currentLayout());
                formData.append("start", toHex(range.start));
                formData.append("end", toHex(range.end));
                formData.append("chunk", String(chunkIndex));

                var response = await fetch("/flash/read", { method: "POST", body: formData });
                var responseText = await response.text();
                if (!response.ok) {
                    setStatus(t("flash.status.http") + " " + response.status +
                        (responseText ? ": " + responseText : ""), true, false);
                    return;
                }

                var payload;
                try {
                    payload = JSON.parse(responseText);
                } catch (error) {
                    setStatus(t("flash.error.parse"), true, false);
                    return;
                }
                if (!payload || !payload.ok) {
                    setStatus(t("flash.status.error") + " " + ((payload && payload.error) || ""), true, false);
                    return;
                }

                var chunkBytes = hexStrToBytes(payload.data || "");
                var offset = chunkIndex * FLASH_READ_CHUNK;
                for (var i = 0; i < chunkBytes.length; i++) hexBytes[offset + i] = chunkBytes[i];

                if (chunkIndex === 0) {
                    selectedByte = hexBytes.length > 0 ? 0 : -1;
                    selectionStart = selectedByte;
                    selectionEnd = selectedByte;
                    renderHexGrid();
                    renderHexViews();
                }
            }

            updatePageControls();
            setStatus(t("flash.status.done") + " (" + human(hexBytes.length) + ")", false, false);
        } catch (error) {
            setStatus(t("flash.status.error") + " " + errText(error), true, false);
        }
    }

    async function doWrite() {
        var target = currentTargetValue();
        if (!target) { setStatus(t("flash.error.no_target"), true, false); return; }
        if (hexBytes.length === 0) { setStatus(t("flash.error.no_data"), true, false); return; }

        var chunks = buildWriteChunks();
        if (chunks.length === 0) {
            setStatus(t("flash.error.no_changes"), true, false);
            return;
        }

        var totalModified = chunks.reduce(function (sum, chunk) { return sum + chunk.count; }, 0);
        var message = t("flash.confirm.write")
            .replace("$1", human(totalModified)).replace("$2", chunks.length);
        if (!confirm(message)) return;

        try {
            var written = 0;
            for (var index = 0; index < chunks.length; index++) {
                var chunk = chunks[index];
                setStatus(t("flash.status.writing") + " (" + (index + 1) + "/" +
                    chunks.length + ")", false, true);

                var parts = [];
                for (var byteIndex = chunk.start; byteIndex <= chunk.end; byteIndex++)
                    parts.push(padHexByte(hexBytes[byteIndex]));

                var formData = new FormData();
                formData.append("op", "write");
                formData.append("storage", "auto");
                formData.append("target", target);
                formData.append("layout", currentLayout());
                formData.append("start", toHex(readBase + chunk.start));
                formData.append("data", parts.join(" "));

                var response = await fetch("/flash/write", { method: "POST", body: formData });
                var responseText = await response.text();
                if (!response.ok) {
                    setStatus(t("flash.status.http") + " " + response.status, true, false);
                    return;
                }

                var payload;
                try {
                    payload = JSON.parse(responseText);
                } catch (error) {
                    setStatus(t("flash.error.parse"), true, false);
                    return;
                }
                if (!payload || !payload.ok) {
                    setStatus(t("flash.status.error") + " " + ((payload && payload.error) || ""), true, false);
                    return;
                }
                written += chunk.count;
            }

            hexModified = new Set();
            renderHexGrid();
            renderHexViews();
            setStatus(t("flash.status.done") + " " + human(written), false, false);
        } catch (error) {
            setStatus(t("flash.status.error") + " " + errText(error), true, false);
        }
    }

    async function doErase() {
        var target = currentTargetValue();
        var option = currentOption();
        if (!target) { setStatus(t("flash.error.no_target"), true, false); return; }

        var range = effectiveRange();
        var size = optionSize(option);
        var deviceName = (option && option.dataset && option.dataset.mtdName) ?
            option.dataset.mtdName :
            (target.indexOf(":") > 0 ? target.slice(target.indexOf(":") + 1) : target);

        if (!range && size === null) {
            setStatus(t("flash.error.bad_range"), true, false);
            return;
        }

        var detail = range ?
            (deviceName + " " + toHex(range.start) + " ~ " + toHex(range.end)) :
            deviceName;

        if (!confirm(t("flash.confirm.erase"))) return;
        if (!confirm(t("flash.confirm.erase_detail")
                .replace("{device}", deviceName).replace("{detail}", detail))) return;

        setStatus(t("flash.status.erasing"), false, true);

        try {
            var formData = new FormData();
            formData.append("op", "erase");
            formData.append("storage", "auto");
            formData.append("target", target);
            formData.append("layout", currentLayout());
            if (range) {
                formData.append("start", toHex(range.start));
                formData.append("end", toHex(range.end));
            }

            var response = await fetch("/flash/erase", { method: "POST", body: formData });
            var responseText = await response.text();
            if (!response.ok) {
                setStatus(t("flash.status.http") + " " + response.status +
                    (responseText ? ": " + responseText : ""), true, false);
                return;
            }

            var payload;
            try {
                payload = JSON.parse(responseText);
            } catch (error) {
                setStatus(t("flash.error.parse"), true, false);
                return;
            }
            if (!payload || !payload.ok) {
                setStatus(t("flash.status.error") + " " + ((payload && payload.error) || ""), true, false);
                return;
            }

            setStatus(t("flash.status.done"), false, false);
        } catch (error) {
            setStatus(t("flash.status.error") + " " + errText(error), true, false);
        }
    }

    /* Upload one restore chunk; resolves with the server payload. */
    function sendRestoreChunk(blob, chunkOffset, chunkEnd, totalSize, rangeStart) {
        return new Promise(function (resolve, reject) {
            var formData = new FormData();
            formData.append("op", "restore");
            formData.append("backup", blob, "restore_chunk.bin");
            var target = currentTargetValue();
            if (target) formData.append("target", target);
            formData.append("layout", currentLayout());
            formData.append("start", toHex(rangeStart + chunkOffset));
            formData.append("end", toHex(rangeStart + chunkEnd));
            formData.append("storage", "auto");

            var xhr = new XMLHttpRequest();
            xhr.upload.onprogress = function (event) {
                if (!event || !event.lengthComputable) return;
                setProgress("flash_restore_bar", (chunkOffset + event.loaded) / totalSize * 100);
            };
            xhr.upload.onload = function () {
                setProgress("flash_restore_bar", (chunkOffset + (chunkEnd - chunkOffset)) / totalSize * 100);
                setStatus(t("flash.status.restoring"), false, true);
            };
            xhr.onreadystatechange = function () {
                if (xhr.readyState !== 4) return;

                if (xhr.status !== 200) {
                    setStatus(t("flash.status.http") + " " + xhr.status +
                        (xhr.responseText ? ": " + xhr.responseText : ""), true, false);
                    setProgress("flash_restore_bar", null);
                    reject(new Error("http"));
                    return;
                }

                var payload;
                try {
                    payload = JSON.parse(xhr.responseText);
                } catch (error) {
                    setStatus(t("flash.error.parse"), true, false);
                    setProgress("flash_restore_bar", null);
                    reject(error);
                    return;
                }

                if (!payload || !payload.ok) {
                    setStatus(t("flash.status.error") + " " + ((payload && payload.error) || ""), true, false);
                    setProgress("flash_restore_bar", null);
                    reject(new Error("bad"));
                    return;
                }

                if (payload.alert) restoreAlert = payload.alert;
                resolve();
            };

            xhr.open("POST", "/flash/restore");
            xhr.send(formData);
        });
    }

    async function doRestore() {
        var fileInput = $("flash_backup");
        var file = fileInput && fileInput.files && fileInput.files.length ? fileInput.files[0] : null;
        if (!file) {
            setStatus(t("flash.error.no_file"), true, false);
            return;
        }

        if (!confirm(t("flash.confirm.restore"))) return;

        try {
            var totalSize = file.size;
            var parsed = parseBackupFilename(file.name);
            var range = effectiveRange();

            /* A backup file carrying its own range wins over the mode. */
            if (parsed) {
                range = { start: parsed.start, end: parsed.end };
                selectTargetByValue(parsed.storage + ":" + parsed.target);
                var startInput = $("flash_start");
                var endInput = $("flash_end");
                if (startInput) startInput.value = toHex(parsed.start);
                if (endInput) endInput.value = toHex(parsed.end);
                selectMode("range");
                updateModeUi();
            }

            if (!range || (range.end - range.start) !== totalSize) {
                setStatus(t("flash.error.bad_range"), true, false);
                return;
            }

            restoreAlert = "";
            setProgress("flash_restore_bar", 0);
            setStatus(t("flash.status.uploading"), false, true);

            var offset = 0;
            while (offset < totalSize) {
                var next = Math.min(offset + FLASH_RESTORE_CHUNK, totalSize);
                await sendRestoreChunk(file.slice(offset, next), offset, next, totalSize, range.start);
                offset = next;
            }

            setProgress("flash_restore_bar", 100);
            setStatus(t("flash.status.restored").replace("$1", restoreAlert || t("flash.status.done")), false, false);
        } catch (error) {
            setStatus(t("flash.status.error") + " " + errText(error), true, false);
        }
    }

    /* ── Page bootstrap ── */
    function wireEvents() {
        var buttons = [
            ["flash_btn_backup", startBackup],
            ["flash_btn_read", doRead],
            ["flash_btn_write", doWrite],
            ["flash_btn_erase", doErase],
            ["flash_btn_jump", jumpToOffset],
            ["flash_btn_restore", doRestore]
        ];

        buttons.forEach(function (entry) {
            var button = $(entry[0]);
            if (button) button.addEventListener("click", entry[1]);
        });

        var prev = $("flash_page_prev");
        if (prev) prev.addEventListener("click", function () { goToPage(currentPage - 1); });

        var next = $("flash_page_next");
        if (next) next.addEventListener("click", function () { goToPage(currentPage + 1); });

        var modeSelect = $("flash_mode");
        if (modeSelect) modeSelect.addEventListener("change", updateModeUi);

        var targetSelect = $("flash_target");
        if (targetSelect) targetSelect.addEventListener("change", function () {
            autoFillRange();
            updateModeUi();
        });

        var startInput = $("flash_start");
        if (startInput) startInput.addEventListener("input", function () {
            updateRangeHint();
            renderHexViews();
        });

        var endInput = $("flash_end");
        if (endInput) endInput.addEventListener("input", updateRangeHint);

        var grid = $("flash_hex_grid");
        if (grid) {
            grid.addEventListener("mousedown", hexGridMouseDown);
            grid.addEventListener("click", hexGridClick);
            grid.addEventListener("scroll", syncScroll);
        }

        var hexInput = $("flash_hex_input");
        if (hexInput) {
            hexInput.addEventListener("keydown", handleHexKey);
            hexInput.addEventListener("input", handleHexInput);
        }

        var fileInput = $("flash_backup");
        if (fileInput) fileInput.addEventListener("change", updateRestoreInfo);
    }

    window.flashInit = function () {
        setStatus("", false, false);
        updateModeUi();
        renderHexViews();
        updatePageControls();
        updateRestoreInfo();
        wireEvents();
        refreshInfo();
    };
})();
