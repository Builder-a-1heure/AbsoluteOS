// ============================================================================
// shell_fs.h — Commandes Ray64 du terminal : `fs ...`, et les raccourcis `cd` et `ls`
// ============================================================================
#pragma once
void cmd_fs(const char *args);
void cmd_cd(const char *args);        // raccourci de `fs cd`
void cmd_ls(const char *args);        // raccourci de `fs ls`
void fs_print_prompt(void);           // affiche l'invite : emplacement courant + "> "
