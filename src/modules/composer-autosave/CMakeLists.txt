set(extra_deps
	evolution-mail-composer
	evolution-shell
)
set(sources
	evolution-composer-autosave.c
	e-autosave-utils.c
	e-autosave-utils.h
	e-composer-autosave.c
	e-composer-autosave.h
	e-composer-registry.c
	e-composer-registry.h
)
set(extra_defines)
set(extra_cflags)
set(extra_incdirs)
set(extra_ldflags)

add_evolution_module(module-composer-autosave
	sources
	extra_deps
	extra_defines
	extra_cflags
	extra_incdirs
	extra_ldflags
)
