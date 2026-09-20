/* ==========================================================================
   main.js - Airoha U-Boot Recovery (Brutalism UI)
   AJAX upload / inline confirm / inline error / language switch / result i18n
   ========================================================================== */

(function (w, d) {
	"use strict";

	/* ---- Element IDs to hide during upload confirmation ----
	 * Includes #section-uboot (its <p> title is NOT a direct child of
	 * #main, so it would otherwise stay visible during confirmation).
	 */
	var UPLOAD_HIDE_IDS = [
		"section-uboot",
		"form-firmware", "form-uboot", "form-initramfs", "form-reboot"
	];

	/* ---- Show #upload-result and hide upload forms ---- */
	function hideUploadForms() {
		for (var i = 0; i < UPLOAD_HIDE_IDS.length; i++) {
			var el = d.getElementById(UPLOAD_HIDE_IDS[i]);
			if (el) el.style.display = "none";
		}
		var titles = d.querySelectorAll("#main > p");
		for (var i = 0; i < titles.length; i++) titles[i].style.display = "none";
		var hrs = d.querySelectorAll("hr");
		for (var j = 0; j < hrs.length; j++) hrs[j].style.display = "none";
	}

	/* ---- Show only the warning block for the given type ---- */
	function showTypeWarnings(fieldName) {
		var ids = ["warn-firmware", "warn-uboot", "warn-initramfs"];
		for (var i = 0; i < ids.length; i++) {
			var el = d.getElementById(ids[i]);
			if (el) el.style.display = (ids[i] === "warn-" + fieldName) ? "block" : "none";
		}
	}

	/* ---- Show inline error, hide warnings + proceed ---- */
	function showUploadError() {
		var warn = d.querySelectorAll(".type-warnings");
		for (var i = 0; i < warn.length; i++) warn[i].style.display = "none";
		var proc = d.getElementById("proceed-section");
		if (proc) proc.style.display = "none";
		var err = d.getElementById("upload-error");
		if (err) err.style.display = "block";
	}

	/* ---- AJAX upload to /upload ----
	 * fieldName: firmware | uboot | initramfs
	 * Server responds: "<size> <md5>" on success (optionally followed by
	 * "key:value" lines, e.g. the BL2 banner for bl2/uboot uploads),
	 * "fail" on validation error
	 * Success: show MD5/size + type-specific warnings + Proceed button
	 * Failure: show inline error with retry button
	 */
	w.brutalismUpload = function (fieldName) {
		var f = d.getElementById("file-" + fieldName);
		if (!f || !f.files || !f.files[0]) return;

		var fd = new FormData();
		fd.append(fieldName, f.files[0]);

		hideUploadForms();

		/* Reset error/proceed visibility */
		var errEl = d.getElementById("upload-error");
		if (errEl) errEl.style.display = "none";
		var procEl = d.getElementById("proceed-section");
		if (procEl) procEl.style.display = "block";

		var x = new XMLHttpRequest();
		x.open("POST", "/upload");
		x.timeout = 300000;
		x.onreadystatechange = function () {
			if (x.readyState !== 4) return;

			var res = d.getElementById("upload-result");
			if (!res) return;

			/* Determine success or failure */
			var ok = (x.status === 200);
			var r = x.responseText || "";
			if (ok && (r === "fail" || r.indexOf(" ") < 0))
				ok = false;

			if (!ok) {
				/* Show inline error */
				var titleEl = d.getElementById("active-type-title");
				if (titleEl) {
					titleEl.setAttribute("data-i18n", "fail.title");
					titleEl.textContent = "\u2588\u2588\u2588 UPGRADE FAILED! SYSTEM ERROR \u2588\u2588\u2588";
				}
				var fi = d.getElementById("fileinfo");
				if (fi) fi.style.display = "none";
				showUploadError();
				res.style.display = "block";
				if (w.i18n) w.i18n.applyTranslations(res);
				return;
			}

			/* Success: show MD5/size + type-specific warnings.
			 * The first response line is "<size> <md5>"; any extra
			 * "key:value" lines (e.g. the BL2 banner reported for
			 * bl2/uboot uploads) are ignored here. */
			var p = r.split("\n")[0].split(" ");
			var m = d.getElementById("md5-value");
			var sz = d.getElementById("size-value");
			if (m) m.textContent = p[1];
			if (sz) sz.textContent = p[0];

			var typeKeys = {
				"firmware": "index.fw_ubi",
				"uboot": "index.uboot",
				"initramfs": "index.initramfs"
			};
			var titleEl2 = d.getElementById("active-type-title");
			if (titleEl2) {
				titleEl2.setAttribute("data-i18n", typeKeys[fieldName] || "");
				var fallback = {
					"firmware": "\u25B6 UPGRADE SYSTEM FIRMWARE (UBI VOLUME)",
					"uboot": "\u25B6 UPGRADE U-BOOT (MTD PARTITION)",
					"initramfs": "\u25B6 UPLOAD BOOT IMAGE (RAM BOOT)"
				};
				titleEl2.textContent = fallback[fieldName] || fieldName;
			}

			showTypeWarnings(fieldName);
			res.style.display = "block";
			if (w.i18n) w.i18n.applyTranslations(res);
		};
		x.send(fd);
	};

	/* ---- Reset: hide #upload-result, show all upload forms ---- */
	w.brutalismResetUpload = function () {
		var res = d.getElementById("upload-result");
		if (res) res.style.display = "none";

		/* Show all forms and titles */
		for (var i = 0; i < UPLOAD_HIDE_IDS.length; i++) {
			var el = d.getElementById(UPLOAD_HIDE_IDS[i]);
			if (el) el.style.display = "";
		}
		var titles = d.querySelectorAll("#main > p");
		for (var i = 0; i < titles.length; i++) titles[i].style.display = "";
		var hrs = d.querySelectorAll("hr");
		for (var j = 0; j < hrs.length; j++) hrs[j].style.display = "";

		/* Reset file inputs */
		var inputs = d.querySelectorAll("input[type=file]");
		for (var k = 0; k < inputs.length; k++) inputs[k].value = "";

		/* Reset error/proceed visibility for next upload */
		var errEl = d.getElementById("upload-error");
		if (errEl) errEl.style.display = "none";
		var procEl = d.getElementById("proceed-section");
		if (procEl) procEl.style.display = "block";
		var fi = d.getElementById("fileinfo");
		if (fi) fi.style.display = "";
	};

	/* ---- Reboot control: navigate to /reboot.html first, then trigger
	 * the actual action from that page once it has finished loading.
	 *   mode="failsafe": reboot back into failsafe mode (sets env then resets)
	 *   mode="boot":     run bootcmd and boot the installed firmware directly
	 *   mode="normal" (default, also for legacy falsy arg): normal reboot
	 * We MUST NOT send the GET /reboot (or /boot) request here because the
	 * device would reset/boot before /reboot.html (its visual feedback
	 * page) could be fetched, leaving the browser with no resources to render.
	 */
	w.brutalismReboot = function (mode) {
		var m = (mode === "failsafe") ? "failsafe" :
			(mode === "boot") ? "boot" : "normal";

		w.location = "/reboot.html?mode=" + m;
	};

	/* ---- Language switcher ---- */
	function initLang() {
		var b = d.getElementById("lang-switch");
		if (!b) return;
		b.addEventListener("click", function () { if (w.i18n) w.i18n.toggleLang(); });
	}

	/* ---- Fetch /version and stamp it on #banner[data-version] ----
	 * Server returns plain text like:
	 *   "U-Boot 2024.10 abcd123-dirty wifi7"
	 * We show it in the banner's ::after pseudo-element via attr(data-version).
	 */
	function fetchVersion() {
		var banner = d.getElementById("banner");
		if (!banner) return;
		var x = new XMLHttpRequest();
		x.open("GET", "/version");
		x.timeout = 3000;
		x.onreadystatechange = function () {
			if (x.readyState !== 4) return;
			if (x.status !== 200 || !x.responseText) return;
			var v = x.responseText.trim();
			if (v) banner.setAttribute("data-version", "[" + v + "]");
		};
		x.send();
	}

	/* ---- Patch result AJAX to re-apply i18n (used by flashing.html) ---- */
	function patchResultLoader() {
		var m = d.getElementById("main");
		if (!m || !w.MutationObserver) return;
		var obs = new w.MutationObserver(function (muts) {
			var added = false;
			for (var i = 0; i < muts.length; i++) {
				if (muts[i].addedNodes.length > 0) { added = true; break; }
			}
			if (!added || !w.i18n) return;
			obs.disconnect();
			w.i18n.applyTranslations(m);
			obs.observe(m, { childList: true, subtree: true });
		});
		obs.observe(m, { childList: true, subtree: true });
	}

	function init() {
		initLang();
		fetchVersion();
		patchResultLoader();
	}

	if (d.readyState === "loading") d.addEventListener("DOMContentLoaded", init);
	else init();

})(window, document);
