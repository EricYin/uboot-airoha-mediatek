# SPDX-License-Identifier: GPL-2.0

# example:
# obj-y += XXXX.o <- object file to generate
# FILE_XXXX.o := YYYY.html <- actual html file to be embedded
# FSPATH_XXXX.o := ZZZ/WWW.EXT <- virtual fs path to be used inside u-boot

# Compression algorithm for the embedded assets (see WEBUI_FAILSAFE_COMPRESS_*).
# The single "%.ext$(WEBUI_FAILSAFE_ZSUF)" pattern rule below is expanded when
# this makefile is read, so only one of the two algorithms is ever wired up.
ifeq ($(CONFIG_WEBUI_FAILSAFE_COMPRESS_NONE),y)
WEBUI_FAILSAFE_ZSUF := .raw
WEBUI_FAILSAFE_ZFLAG := --none
else
WEBUI_FAILSAFE_ZSUF := .gz
WEBUI_FAILSAFE_ZFLAG := --gzip
endif

obj-y += 404.o
FILE_404.o := .min/404.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_404.o := 404.html

obj-$(CONFIG_WEBUI_FAILSAFE_LAYOUT_FIP) += bl2.o
FILE_bl2.o := .min/bl2.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_bl2.o := bl2.html

obj-$(CONFIG_WEBUI_FAILSAFE_LAYOUT_CHAINLOADER) += chainloader.o
FILE_chainloader.o := .min/chainloader.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_chainloader.o := chainloader.html

obj-y += booting.o
FILE_booting.o := .min/booting.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_booting.o := booting.html

obj-$(CONFIG_WEBUI_FAILSAFE_CONSOLE) += console.o
FILE_console.o := .min/console.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_console.o := console.html

obj-$(CONFIG_WEBUI_FAILSAFE_ENV) += env.o
FILE_env.o := .min/env.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_env.o := env.html

obj-y += fail.o
FILE_fail.o := .min/fail.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_fail.o := fail.html

obj-$(CONFIG_WEBUI_FAILSAFE_GPT) += gpt.o
FILE_gpt.o := .min/gpt.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_gpt.o := gpt.html

obj-y += flashing.o
FILE_flashing.o := .min/flashing.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_flashing.o := flashing.html

obj-$(CONFIG_WEBUI_FAILSAFE_FLASH) += flash.o
FILE_flash.o := .min/flash.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_flash.o := flash.html

obj-y += index.o
FILE_index.o := .min/index.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_index.o := index.html

obj-y += initramfs.o
FILE_initramfs.o := .min/initramfs.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_initramfs.o := initramfs.html

obj-y += reboot.o
FILE_reboot.o := .min/reboot.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_reboot.o := reboot.html

obj-$(CONFIG_WEBUI_FAILSAFE_ENV) += settings.o
FILE_settings.o := .min/settings.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_settings.o := settings.html

obj-$(CONFIG_WEBUI_FAILSAFE_UBI) += ubi.o
FILE_ubi.o := .min/ubi.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_ubi.o := ubi.html

obj-$(CONFIG_WEBUI_FAILSAFE_SIMG) += simg.o
FILE_simg.o := .min/simg.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_simg.o := simg.html

obj-$(CONFIG_WEBUI_FAILSAFE_LAYOUT_UBOOT) += uboot.o
FILE_uboot.o := .min/uboot.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_uboot.o := uboot.html

obj-$(CONFIG_WEBUI_FAILSAFE_LAYOUT_FIP) += fip.o
FILE_fip.o := .min/fip.html$(WEBUI_FAILSAFE_ZSUF)
FSPATH_fip.o := fip.html

obj-y += favicon.o
FILE_favicon.o := .min/favicon.svg$(WEBUI_FAILSAFE_ZSUF)
FSPATH_favicon.o := favicon.svg

obj-y += style.o
FILE_style.o := .min/style.css$(WEBUI_FAILSAFE_ZSUF)
FSPATH_style.o := style.css

obj-$(CONFIG_WEBUI_FAILSAFE_CONSOLE) += console_js.o
FILE_console_js.o := .min/console_js.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_console_js.o := console_js.js

obj-$(CONFIG_WEBUI_FAILSAFE_ENV) += env_js.o
FILE_env_js.o := .min/env_js.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_env_js.o := env_js.js

obj-$(CONFIG_WEBUI_FAILSAFE_FLASH) += flash_js.o
FILE_flash_js.o := .min/flash_js.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_flash_js.o := flash_js.js

obj-$(CONFIG_WEBUI_FAILSAFE_I18N) += i18n.o
FILE_i18n.o := .min/i18n.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_i18n.o := i18n.js

obj-y += main.o
FILE_main.o := .min/main.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_main.o := main.js

obj-$(CONFIG_WEBUI_FAILSAFE_ENV) += settings_js.o
FILE_settings_js.o := .min/settings_js.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_settings_js.o := settings_js.js

obj-$(CONFIG_WEBUI_FAILSAFE_UBI) += ubi_js.o
FILE_ubi_js.o := .min/ubi_js.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_ubi_js.o := ubi_js.js

obj-$(CONFIG_WEBUI_FAILSAFE_SIMG) += simg_js.o
FILE_simg_js.o := .min/simg_js.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_simg_js.o := simg_js.js

obj-y += theme.o
FILE_theme.o := .min/theme.js$(WEBUI_FAILSAFE_ZSUF)
FSPATH_theme.o := theme.js

# customized build rules
strip_path = $(subst /,_,$(subst -,_,$(subst .,_,$(1))))
WEBUI_FAILSAFE_JS_MINIFY_SCRIPT = $(srctree)/$(src)/../minify-js.cjs

WEBUI_FAILSAFE_CSS_MINIFY_SCRIPT = $(srctree)/$(src)/../minify-css.cjs
WEBUI_FAILSAFE_HTML_MINIFY_SCRIPT = $(srctree)/$(src)/../minify-html.cjs
WEBUI_FAILSAFE_GZIP_SCRIPT = $(srctree)/$(src)/../gzip-compress.cjs
WEBUI_FAILSAFE_I18N_FILTER_SCRIPT = $(srctree)/$(src)/../filter-i18n.cjs

# Languages kept in i18n.js (see the WEBUI_FAILSAFE_I18N_* options).
WEBUI_FAILSAFE_I18N_LANGS := $(strip \
	$(if $(CONFIG_WEBUI_FAILSAFE_I18N_EN),en) \
	$(if $(CONFIG_WEBUI_FAILSAFE_I18N_ZH_CN),zh zh-cn) \
	$(if $(CONFIG_WEBUI_FAILSAFE_I18N_RU),ru))

# Namespaces of the pages that are not built.  The list mirrors the
# obj-$(CONFIG_...) gating of the assets above: a page that is left out
# contributes no strings, which is where most of the dictionary comes from.
# Keys an embedded asset still references are kept anyway (see the filter
# script), so this only has to name the namespaces, not individual keys.
WEBUI_FAILSAFE_I18N_DROP_NS := $(strip \
	$(if $(CONFIG_WEBUI_FAILSAFE_CONSOLE),,console) \
	$(if $(CONFIG_WEBUI_FAILSAFE_ENV),,env settings) \
	$(if $(CONFIG_WEBUI_FAILSAFE_UBI),,ubi) \
	$(if $(CONFIG_WEBUI_FAILSAFE_SIMG),,simg) \
	$(if $(CONFIG_WEBUI_FAILSAFE_FLASH),,flash) \
	$(if $(CONFIG_WEBUI_FAILSAFE_GPT),,gpt) \
	$(if $(CONFIG_WEBUI_FAILSAFE_LAYOUT_UBOOT),,uboot) \
	$(if $(CONFIG_WEBUI_FAILSAFE_LAYOUT_CHAINLOADER),,chainloader))

# The assets that end up in this build - their source files, taken from the
# FSPATH of every object listed above.  What they mention is what keeps a
# translation alive.
#
# The dictionary itself is left out: every key is a literal in it, so
# scanning i18n.js would make the whole dictionary look referenced and the
# filter would never drop anything.
WEBUI_FAILSAFE_I18N_ASSETS := $(foreach o,$(filter-out i18n.o,$(obj-y)),\
	$(if $(FSPATH_$(o)),$(src)/$(FSPATH_$(o))))

ifeq ($(CONFIG_WEBUI_FAILSAFE_I18N_TRIM),y)
WEBUI_FAILSAFE_I18N_TRIM_ARGS := \
	--drop-ns="$(WEBUI_FAILSAFE_I18N_DROP_NS)" \
	--assets="$(WEBUI_FAILSAFE_I18N_ASSETS)"
else
WEBUI_FAILSAFE_I18N_TRIM_ARGS :=
endif

quiet_cmd_i18n_filter = I18N    $@
cmd_i18n_filter = mkdir -p $(dir $@) && node $(WEBUI_FAILSAFE_I18N_FILTER_SCRIPT) --langs="$(WEBUI_FAILSAFE_I18N_LANGS)" $(WEBUI_FAILSAFE_I18N_TRIM_ARGS) $< $@

quiet_cmd_minify_css = CLEANCSS $@
cmd_minify_css = mkdir -p $(dir $@) && node $(WEBUI_FAILSAFE_CSS_MINIFY_SCRIPT) $< $@

quiet_cmd_minify_html = HTMLMIN $@
cmd_minify_html = mkdir -p $(dir $@) && node $(WEBUI_FAILSAFE_HTML_MINIFY_SCRIPT) $< $@

quiet_cmd_minify_js = UGLIFY  $@
cmd_minify_js = mkdir -p $(dir $@) && node $(WEBUI_FAILSAFE_JS_MINIFY_SCRIPT) $< $@

quiet_cmd_fsdata_gzembed = COMPRESS $@
cmd_fsdata_gzembed = node $(WEBUI_FAILSAFE_GZIP_SCRIPT) $(WEBUI_FAILSAFE_ZFLAG) $< $@

quiet_cmd_as_o_html = AS      $@
cmd_as_o_html = $(CC) $(a_flags) -c -o $@ \
		-DDATA_SECT_NAME="\".rodata.fsdata.$(call strip_path,$(FSPATH_$(@F)))\"" \
		-DDATA_OBJ_FILE="\"$(srctree)/$(obj)/$(FILE_$(@F))\"" \
		-DDATA_OBJ_NAME="fsdata_$(call strip_path,$(FSPATH_$(@F)))" \
		-DDATA_SIZE_NAME="fsdata_size_$(call strip_path,$(FSPATH_$(@F)))" \
		-DDATA_SECT_FSPATH_NAME="\".rodata.fsdata.path.$(call strip_path,$(FSPATH_$(@F)))\"" \
		-DDATA_OBJ_FSPATH_NAME="fsdata_path_$(call strip_path,$(FSPATH_$(@F)))" \
		-DDATA_OBJ_FSPATH="\"$(FSPATH_$(@F))\"" \
		-DUBOOT_LIST_SECT_NAME="\"__u_boot_list_2_fs_2_$(call strip_path,$(FSPATH_$(@F)))\"" \
		-DUBOOT_LIST_NAME="fsdata_ulist_$(call strip_path,$(FSPATH_$(@F)))" \
		$(srctree)/$(src)/embed.S

# Step 0: drop the languages that are not configured.
#
# These two rules are explicit (non-pattern) so that they win over the
# generic "$(obj)/.min/%.js: $(src)/%.js" rule below for i18n.js.
$(obj)/.min/i18n.js: $(obj)/.i18n/i18n.js $(WEBUI_FAILSAFE_JS_MINIFY_SCRIPT) FORCE
	$(call cmd,force_checksrc)
	$(call if_changed,minify_js)

# The assets are prerequisites as well ($< stays the dictionary): adding or
# removing a key reference in a page changes what the filter has to keep.
$(obj)/.i18n/%.js: $(src)/%.js $(WEBUI_FAILSAFE_I18N_FILTER_SCRIPT) $(WEBUI_FAILSAFE_I18N_ASSETS) FORCE
	$(call if_changed,i18n_filter)

# Step 1: Minify source files
$(obj)/.min/%.html: $(src)/%.html $(WEBUI_FAILSAFE_HTML_MINIFY_SCRIPT) FORCE
	$(call cmd,force_checksrc)
	$(call if_changed,minify_html)

$(obj)/.min/%.css: $(src)/%.css $(WEBUI_FAILSAFE_CSS_MINIFY_SCRIPT) FORCE
	$(call cmd,force_checksrc)
	$(call if_changed,minify_css)

$(obj)/.min/%.js: $(src)/%.js $(WEBUI_FAILSAFE_JS_MINIFY_SCRIPT) FORCE
	$(call cmd,force_checksrc)
	$(call if_changed,minify_js)

# Step 2: GZip-compress minified files
$(obj)/.min/%.html$(WEBUI_FAILSAFE_ZSUF): $(obj)/.min/%.html $(WEBUI_FAILSAFE_GZIP_SCRIPT) FORCE
	$(call if_changed,fsdata_gzembed)

$(obj)/.min/%.css$(WEBUI_FAILSAFE_ZSUF): $(obj)/.min/%.css $(WEBUI_FAILSAFE_GZIP_SCRIPT) FORCE
	$(call if_changed,fsdata_gzembed)

$(obj)/.min/%.js$(WEBUI_FAILSAFE_ZSUF): $(obj)/.min/%.js $(WEBUI_FAILSAFE_GZIP_SCRIPT) FORCE
	$(call if_changed,fsdata_gzembed)

$(obj)/.min/%.svg$(WEBUI_FAILSAFE_ZSUF): $(src)/%.svg $(WEBUI_FAILSAFE_GZIP_SCRIPT) FORCE
	$(call if_changed,fsdata_gzembed)

# Step 3: Embed GZip-compressed files into object files
$(obj)/%.o: $(obj)/.min/%.html$(WEBUI_FAILSAFE_ZSUF) FORCE
	$(call if_changed_dep,as_o_html)

$(obj)/%.o: $(obj)/.min/%.css$(WEBUI_FAILSAFE_ZSUF) FORCE
	$(call if_changed_dep,as_o_html)

$(obj)/%.o: $(obj)/.min/%.js$(WEBUI_FAILSAFE_ZSUF) FORCE
	$(call if_changed_dep,as_o_html)

$(obj)/favicon.o: $(obj)/.min/favicon.svg$(WEBUI_FAILSAFE_ZSUF) FORCE
	$(call if_changed_dep,as_o_html)

clean-files += .min/*
clean-files += .i18n/*
