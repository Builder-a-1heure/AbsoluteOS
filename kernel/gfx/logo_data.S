// ============================================================================
// logo_data.S — Embarque le logo de démarrage (texte UTF-8 avec couleurs ANSI)
// Pour changer de logo, remplace kernel/gfx/logo/logo.ansi : rien d'autre à modifier.
// ============================================================================
    .section .rodata
    .global logo_ansi_start
    .hidden logo_ansi_start
logo_ansi_start:
    .incbin "kernel/gfx/logo/logo.ansi"
    .global logo_ansi_end
    .hidden logo_ansi_end
logo_ansi_end:
