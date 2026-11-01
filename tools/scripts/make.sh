#!/bin/bash
# ============================================================
#  OS arm64 (UEFI) — Script de build automatique (basé sur TetraOS)
#
#  Détecte automatiquement tous les .c et .S dans COMPILE_DIRS.
#  Seule contrainte fixe : boot.S doit être linkée EN PREMIER
#  (il contient l'en-tête PE que le firmware UEFI lit au début du fichier).
#
#  Prérequis :
#    macOS : brew install aarch64-elf-gcc aarch64-elf-binutils qemu
#    Linux : apt install gcc-aarch64-linux-gnu qemu-system-arm qemu-efi-aarch64
#
#  Usage :
#    chmod +x tools/scripts/make.sh
#    ./tools/scripts/make.sh             → compile + lance QEMU
#    ./tools/scripts/make.sh --no-run    → compile seulement
#    ./tools/scripts/make.sh --headless  → QEMU sans fenêtre (série seule, Ctrl-A puis X pour quitter)
#    ./tools/scripts/make.sh --verbose   → affiche chaque fichier trouvé
#    ./tools/scripts/make.sh --fresh     → recrée le disque UEFI (esp/)
#
#  Variables optionnelles : SMP=8 MEM=2G FW=/chemin/firmware.fd
#
#  Comportement sur esp/ :
#    - esp/ est le "disque" FAT présenté au firmware (équivalent de os.img).
#    - Il ne contient que EFI/BOOT/BOOTAA64.EFI, réécrit à chaque build.
#    - --fresh efface tout esp/ avant (utile si tu y ajoutes des fichiers de test).
# ============================================================

set -e

# ============================================================
# RACINE DU PROJET
# Se déplacer dans la racine du projet (deux niveaux au-dessus
# de tools/scripts/) quel que soit le répertoire courant.
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"
echo "==> Racine projet : $PROJECT_ROOT"

# ============================================================
# CONFIGURATION
# ============================================================
QEMU="qemu-system-aarch64"
SMP="${SMP:-4}"                          # cœurs de la VM
MEM="${MEM:-1G}"                         # RAM de la VM

KERNEL_DIR="kernel"
BUILD="tools/build"
OBJ="$BUILD/compilation"
ENTRY_SRC="kernel/boot/boot.S"           # doit être linkée EN PREMIER
LINKER_SCRIPT="$KERNEL_DIR/boot/linker.ld"
OUT_ELF="$BUILD/kernel.elf"
OUT_MAP="$BUILD/kernel.map"
OUT_EFI="$BUILD/BOOTAA64.EFI"            # nom imposé par UEFI pour le boot auto en arm64
ESP="esp"                                # disque FAT vu par le firmware

# Sous-dossiers à compiler — LISTE EXPLICITE.
# Le find ne sort jamais de ces dossiers (donc ne mets pas "kernel" seul :
# il contiendrait kernel/boot et tout serait compilé deux fois).
# Décommente / ajoute une ligne quand tu crées le dossier.
COMPILE_DIRS=(
    "kernel/boot"
    "kernel/core"
    "kernel/drivers"
    "kernel/gfx"
    "kernel/fs"
    "kernel/lib"
    "kernel/mem"
    "os/shell"
#   "os/ui"
#   "os/apps"
)

# Fichier de liste transmis au linker
OBJ_LIST="$OBJ/link_objects.txt"

# -fpie                : image relocalisable (UEFI nous charge où il veut)
# -mgeneral-regs-only  : pas de NEON/FPU
# -mstrict-align       : aucun accès mémoire non aligné (plante sur mémoire Device)
CFLAGS="-ffreestanding -O2 -Wall -Wextra -nostdlib -g \
    -fpie -fno-plt -fno-stack-protector -fvisibility=hidden \
    -mgeneral-regs-only -mstrict-align -fno-builtin \
    -Ikernel/boot \
    -Ikernel/core \
    -Ikernel/drivers \
    -Ikernel/fs \
    -Ikernel/gfx \
    -Ikernel/lib \
    -Ikernel/mem \
    -Ios/shell \
    -Ikernel"

LDFLAGS="-nostdlib -pie -Wl,--no-dynamic-linker -Wl,-z,norelro -Wl,-z,noexecstack \
    -Wl,-Bsymbolic -Wl,-T,$LINKER_SCRIPT -Wl,-Map,$OUT_MAP"

VERBOSE=0; NO_RUN=0; FRESH=0; HEADLESS=0
for arg in "$@"; do
    [[ "$arg" == "--verbose"  ]] && VERBOSE=1
    [[ "$arg" == "--no-run"   ]] && NO_RUN=1
    [[ "$arg" == "--fresh"    ]] && FRESH=1
    [[ "$arg" == "--headless" ]] && HEADLESS=1
done

# ============================================================
# COULEURS
# ============================================================
RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[0;33m'
BLU='\033[0;34m'; CYN='\033[0;36m'; RST='\033[0m'
info() { echo -e "${BLU}$*${RST}"; }
ok()   { echo -e "${GRN}$*${RST}"; }
warn() { echo -e "${YEL}$*${RST}"; }
err()  { echo -e "${RED}$*${RST}"; }

# ============================================================
# [0] VÉRIFICATION DES OUTILS
# ============================================================
info "==> Vérification des outils..."

# Préfixe du cross-compilateur : premier trouvé parmi les usuels
CROSS=""
for p in aarch64-elf- aarch64-none-elf- aarch64-linux-gnu-; do
    if command -v "${p}gcc" &>/dev/null; then CROSS="$p"; break; fi
done
if [[ -z "$CROSS" ]]; then
    err "  ERREUR : aucun cross-compilateur arm64 trouvé."
    warn "  Installe : brew install aarch64-elf-gcc aarch64-elf-binutils qemu"
    exit 1
fi
GCC="${CROSS}gcc"; OBJCOPY="${CROSS}objcopy"

missing=0
for tool in "$GCC" "$OBJCOPY" "$QEMU"; do
    if ! command -v "$tool" &>/dev/null; then
        err "  ERREUR : '$tool' introuvable."; missing=1
    fi
done
[[ $missing -eq 1 ]] && exit 1

# Firmware UEFI pour QEMU : variable FW, sinon premier chemin existant
if [[ -z "$FW" ]]; then
    BREW_PREFIX="$(brew --prefix 2>/dev/null || true)"
    for f in \
        "$BREW_PREFIX/share/qemu/edk2-aarch64-code.fd" \
        /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
        /usr/local/share/qemu/edk2-aarch64-code.fd \
        /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
        /usr/share/AAVMF/AAVMF_CODE.fd; do
        [[ -f "$f" ]] && { FW="$f"; break; }
    done
fi
if [[ -z "$FW" || ! -f "$FW" ]]; then
    err "  ERREUR : firmware UEFI arm64 (edk2) introuvable. Précise-le : FW=/chemin $0"
    exit 1
fi
ok "    Compilateur : $GCC   Firmware : $FW"

# ============================================================
# [1] NETTOYAGE (compilation seulement — esp/ préservé sauf --fresh)
# ============================================================
echo ""
info "[1/6] Nettoyage des fichiers de compilation..."
rm -rf "$BUILD"

# ============================================================
# [2] CRÉATION DU DOSSIER COMPILATION
# ============================================================
info "[2/6] Création du dossier $OBJ..."
mkdir -p "$OBJ"

# ============================================================
# [3] DÉTECTION ET COMPILATION AUTOMATIQUE DES .c et .S
# ============================================================
info "[3/6] Détection des sources dans : ${COMPILE_DIRS[*]}"

# Collecter les .c/.S uniquement dans les dossiers de COMPILE_DIRS.
ALL_SRC=()
for dir in "${COMPILE_DIRS[@]}"; do
    if [ -d "$dir" ]; then
        while IFS= read -r f; do
            ALL_SRC+=("$f")
        done < <(find "$dir" \( -name "*.c" -o -name "*.S" \) -print | sort)
    else
        warn "  ATTENTION : dossier '$dir' introuvable, ignoré."
    fi
done

ENTRY_OBJ=""
: > "$OBJ_LIST.rest"
compiled_count=0

echo ""
echo "  Sources trouvées :"

for src in "${ALL_SRC[@]}"; do
    base="${src//\//_}.o"                # ex: kernel_boot_boot.S.o (pas de collision foo.c / foo.S)
    obj="$OBJ/$base"

    if [[ $VERBOSE -eq 1 ]]; then
        echo -e "  ${CYN}→${RST} $src"
    else
        echo "  $src"
    fi

    if ! $GCC $CFLAGS -c "$src" -o "$obj"; then
        err "ERREUR de compilation dans $src"; exit 1
    fi
    compiled_count=$((compiled_count + 1))

    if [[ "$src" == "$ENTRY_SRC" ]]; then
        ENTRY_OBJ="$obj"
    else
        echo "$obj" >> "$OBJ_LIST.rest"
    fi
done

echo ""
ok "  → $compiled_count fichier(s) compilé(s)."

if [[ -z "$ENTRY_OBJ" ]]; then
    err "ERREUR : point d'entrée '$ENTRY_SRC' introuvable !"
    exit 1
fi

echo "$ENTRY_OBJ"  >  "$OBJ_LIST"
cat "$OBJ_LIST.rest" >> "$OBJ_LIST"
rm -f "$OBJ_LIST.rest"

if [[ $VERBOSE -eq 1 ]]; then
    echo ""
    echo "  Ordre de linkage :"
    cat "$OBJ_LIST" | while read -r line; do echo "    $line"; done
fi

# ============================================================
# [4] LINKAGE
# ============================================================
info "[4/6] Linkage du kernel..."

OBJ_ARGS=()
while IFS= read -r line; do
    [[ -n "$line" ]] && OBJ_ARGS+=("$line")
done < "$OBJ_LIST"

# (on masque les warnings ld habituels : segment RWX voulu, build-id ignoré)
$GCC $CFLAGS $LDFLAGS "${OBJ_ARGS[@]}" -o "$OUT_ELF" \
    2> >(grep -v -E "RWX permissions|build-id" >&2)

info "[4/6] Extraction de l'image EFI..."
# L'en-tête PE est écrit dans boot.S : on aplatit simplement l'ELF en binaire brut.
$OBJCOPY -O binary "$OUT_ELF" "$OUT_EFI"

EFI_SIZE=$(wc -c < "$OUT_EFI")
ok "  BOOTAA64.EFI : $((EFI_SIZE / 1024)) KB"

if [[ "$(head -c 2 "$OUT_EFI")" != "MZ" ]]; then
    err "ERREUR : l'image ne commence pas par l'en-tête PE ('MZ')."
    err "  Vérifie que boot.S est bien linkée en premier."
    exit 1
fi

# ============================================================
# [5] DISQUE UEFI (esp/)
# ============================================================
echo ""
if [[ $FRESH -eq 1 ]]; then
    info "[5/6] --fresh : recréation complète de $ESP/..."
    rm -rf "$ESP"
else
    info "[5/6] Mise à jour de $ESP/ (seul BOOTAA64.EFI est réécrit)..."
fi
mkdir -p "$ESP/EFI/BOOT"
cp "$OUT_EFI" "$ESP/EFI/BOOT/BOOTAA64.EFI"   # chemin que le firmware lance tout seul

# ============================================================
# [6] LANCEMENT QEMU
# ============================================================
if [[ $NO_RUN -eq 1 ]]; then
    echo ""; ok "Build OK ! (QEMU non lancé)"; exit 0
fi

# Firmware : gros .fd brew (64 Mo) => pflash ; petit fichier => -bios
if [[ "$FW" == *edk2-aarch64-code.fd ]]; then
    FW_ARGS=(-drive "if=pflash,format=raw,file=$FW,readonly=on")
else
    FW_ARGS=(-bios "$FW")
fi

if [[ $HEADLESS -eq 1 ]]; then
    DISPLAY_ARGS=(-display none -serial mon:stdio)
elif [[ "$(uname)" == "Darwin" ]]; then
    DISPLAY_ARGS=(-display cocoa,zoom-to-fit=on -serial stdio)
else
    DISPLAY_ARGS=(-serial stdio)
fi

info "[6/6] Lancement de QEMU (${SMP} cœurs, ${MEM})..."

$QEMU -M virt -cpu cortex-a72 -smp "$SMP" -m "$MEM" "${FW_ARGS[@]}" \
    -device ramfb \
    -drive if=none,id=hd,format=raw,file=fat:rw:"$ESP" \
    -device virtio-blk-pci,drive=hd \
    -device qemu-xhci -device usb-kbd \
    "${DISPLAY_ARGS[@]}" \
    -d guest_errors

echo ""; ok "Build OK !"
