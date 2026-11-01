// ============================================================================
// font_data.S — Embarque le fichier .psf dans l'image du noyau.
// Le fichier est inclus tel quel (.incbin) : pour changer de police, remplace
// kernel/gfx/fonts/font.psf, rien d'autre à modifier. Le noyau l'analyse au
// démarrage (font.c). Chemin relatif à la racine du projet (là où make.sh compile).
// ============================================================================
    .section .rodata                   // données en lecture seule
    .balign 4
    .global font_psf_start
    .hidden font_psf_start             // visibilité "cachée" : accès direct sans GOT (image PIE)
font_psf_start:                        // début du fichier PSF
    .incbin "kernel/gfx/fonts/font.psf"
    .global font_psf_end
    .hidden font_psf_end
font_psf_end:                          // fin du fichier PSF (taille = end - start)
