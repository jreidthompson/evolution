set(extra_deps
	evolution-mail
	evolution-mail-composer
)
set(sources
	evolution-module-webkit-editor.c
	e-webkit-editor-extension.c
	e-webkit-editor-extension.h
	e-webkit-editor.c
	e-webkit-editor.h
)
set(extra_defines
	-DEVOLUTION_WEB_EXTENSIONS_WEBKIT_EDITOR_DIR=\"${webextensionswebkiteditordir}\"
)
set(extra_cflags)
set(extra_incdirs)
set(extra_ldflags)

add_evolution_module(module-webkit-editor
	sources
	extra_deps
	extra_defines
	extra_cflags
	extra_incdirs
	extra_ldflags
)

add_subdirectory(web-extension)
