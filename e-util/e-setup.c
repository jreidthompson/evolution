/*
 * Sets up the ~/evolution directory
 *
 * Author:
 *    Miguel de Icaza (miguel@kernel.org)
 *
 * (C) 2000 Helix Code, Inc. http://www.helixcode.com
 */
#include <config.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gnome.h>
#include "e-setup.h"

char *evolution_dir = NULL;
char *evolution_folders_dir = NULL;
char *evolution_private = NULL;
char *evolution_public = NULL;

gboolean 
e_setup_base_dir (void)
{
	struct stat s;
	
	evolution_dir = g_concat_dir_and_file (g_get_home_dir (), "evolution");
	
	if (stat (evolution_dir, &s) == -1){
		if (mkdir (evolution_dir, 0600) == -1){
			return FALSE;
		}
	} else {
		if (!S_ISDIR (s.st_mode)){
			char *msg;

			g_error ("Finish implementing this");
			
			msg = g_strdup_printf (
				  _("Evolution detected that the file `%s' is a not a directory.\n"
				    "\n"
				    "Evolution can rename the file, delete the file or shutdown and\n"
				    "let you fix the problem."));
			return FALSE;
		}
	}

	evolution_folders_dir = g_concat_dir_and_file (evolution_dir, "folders");
	mkdir (evolution_folders_dir, 0600);
		
	return TRUE;
}

