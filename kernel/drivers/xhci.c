// ============================================================================
// xhci.c — Driver xHCI minimal, en scrutation (polling), pour claviers USB
//
// Principe du xHCI : le CPU et le contrôleur communiquent par des anneaux de
// TRB (blocs de 16 octets) en RAM :
//   - anneau de COMMANDES  : le CPU y écrit des ordres (activer un slot, adresser...)
//   - anneau d'ÉVÉNEMENTS  : le contrôleur y écrit les résultats
//   - anneaux de TRANSFERT : un par endpoint (EP0 pour la configuration, puis
//                            l'endpoint "interruption" du clavier)
// On prévient le contrôleur en écrivant dans un registre "doorbell".
// Un bit "cycle" dans chaque TRB indique à qui appartient l'entrée (producteur/consommateur).
//
// LIMITES : contrôleur PCIe uniquement (pas les xHCI décrits seulement en
// device-tree), pas de hub USB, mémoire DMA supposée cohérente avec le cache.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "xhci.h"
#include "timer.h"
#include "kprintf.h"
#include "keyboard.h"

extern void *memset(void *s, int c, size_t n);

#define DSB() __asm__ volatile("dsb sy" ::: "memory")    // barrière : tout est écrit avant de prévenir le contrôleur

// ---- Types de TRB et bits utiles ------------------------------------------
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDR_DEV    11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_EV_TRANSFER 32
#define TRB_EV_CMD      33

#define TRB_TC   (1u << 1)    // Toggle Cycle (TRB de lien)
#define TRB_ISP  (1u << 2)    // Interrupt on Short Packet
#define TRB_IOC  (1u << 5)    // Interrupt On Completion : génère un événement
#define TRB_IDT  (1u << 6)    // Immediate Data (setup packet dans le TRB lui-même)
#define TRB_DIR  (1u << 16)   // direction IN pour les étapes data/status

typedef struct { uint64_t param; uint32_t status; uint32_t control; } __attribute__((aligned(16))) Trb;

// Anneau de TRB côté producteur (CPU) : commandes et transferts
typedef struct { Trb *trbs; uint32_t n, enq, pcs; } Ring;

#define MAX_DEV   4
#define EV_N      64
#define CMD_N     32
#define EP_N      32
#define MAX_SCRATCH 32

// ---- Mémoire DMA statique (en .bss) ----------------------------------------
static uint64_t g_dcbaa[256]            __attribute__((aligned(64)));          // table slot -> contexte appareil
static Trb      g_cmd_ring[CMD_N]       __attribute__((aligned(64)));
static Trb      g_ev_ring[EV_N]         __attribute__((aligned(64)));
static struct { uint64_t base; uint32_t size, rsvd; } g_erst __attribute__((aligned(64)));  // table des segments d'événements
static uint64_t g_scratch_arr[MAX_SCRATCH] __attribute__((aligned(64)));
static uint8_t  g_scratch_pg[MAX_SCRATCH][4096] __attribute__((aligned(4096)));
static uint8_t  g_devctx[MAX_DEV][2048] __attribute__((aligned(64)));          // contexte "sortie" (écrit par le contrôleur)
static uint8_t  g_inctx[MAX_DEV][2304]  __attribute__((aligned(64)));          // contexte "entrée" (écrit par nous)
static Trb      g_ep0_ring[MAX_DEV][EP_N]   __attribute__((aligned(64)));
static Trb      g_int_ring[MAX_DEV][EP_N]   __attribute__((aligned(64)));
static uint8_t  g_report[MAX_DEV][64]   __attribute__((aligned(64)));          // rapport clavier (8 octets utiles)
static uint8_t  g_desc[256]             __attribute__((aligned(64)));          // descripteurs USB reçus

// ---- État du driver ---------------------------------------------------------
static volatile uint8_t *g_cap, *g_op, *g_rt, *g_db;   // zones de registres
static uint32_t g_csz;                                   // taille d'un contexte : 32 ou 64 octets
static Ring     g_cmd;
static uint32_t g_ev_deq, g_ev_ccs = 1;                  // position et cycle de lecture de l'anneau d'événements

typedef struct { int used; uint8_t slot, port, speed, dci; Ring ep0, intr; } Dev;
static Dev g_dev[MAX_DEV];

// résultats déposés par pump_events()
static volatile int g_cmd_done, g_cmd_code, g_cmd_slot;
static volatile int g_xfer_done, g_xfer_code;

// ---- Accès registres ---------------------------------------------------------
static uint32_t r32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static void w32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static void w64(volatile uint8_t *b, uint32_t off, uint64_t v) { w32(b, off, (uint32_t)v); w32(b, off + 4, (uint32_t)(v >> 32)); }

// Attend que (registre & mask) == want, au plus `ms` millisecondes ; renvoie 1 si OK
static int wait_reg(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms) {
    uint64_t t = timer_ms();
    while ((r32(b, off) & mask) != want) if (timer_ms() - t > ms) return 0;
    return 1;
}

// ---- Anneaux producteur ------------------------------------------------------
// Prépare un anneau de n TRB : tout à zéro, le dernier est un lien qui reboucle au début
static void ring_init(Ring *r, Trb *mem, uint32_t n) {
    memset(mem, 0, n * sizeof(Trb));
    r->trbs = mem; r->n = n; r->enq = 0; r->pcs = 1;
    mem[n - 1].param = (uint64_t)mem;                 // lien vers le début
    mem[n - 1].control = (TRB_LINK << 10) | TRB_TC;   // TC : le contrôleur inverse son cycle en le suivant
}

// Ajoute un TRB. Le bit de cycle est écrit EN DERNIER : c'est lui qui "donne" l'entrée au contrôleur.
static void ring_push(Ring *r, uint64_t param, uint32_t status, uint32_t control) {
    volatile Trb *t = &r->trbs[r->enq];
    t->param = param;
    t->status = status;
    DSB();
    t->control = control | r->pcs;
    if (++r->enq == r->n - 1) {                       // arrivé sur le TRB de lien
        volatile Trb *l = &r->trbs[r->enq];
        l->control = (l->control & ~1u) | r->pcs;     // on le donne au contrôleur avec le cycle courant
        r->enq = 0;
        r->pcs ^= 1;                                  // au tour suivant, le cycle est inversé
    }
}

// Sonne à la porte du contrôleur : target 0 = commandes ; slot N + target = endpoint (DCI)
static void doorbell(uint32_t slot, uint32_t target) { DSB(); w32(g_db, slot * 4, target); }

// ---- Anneau d'événements -----------------------------------------------------
static void on_transfer_event(uint32_t slot, uint32_t dci, uint32_t code) {
    for (int i = 0; i < MAX_DEV; i++) {
        Dev *d = &g_dev[i];
        if (d->used && d->slot == slot && d->dci && d->dci == dci) {   // endpoint clavier
            if (code == 1 || code == 13) kbd_report(g_report[i]);      // 1 = succès, 13 = paquet court
            memset(g_report[i], 0, 8);
            ring_push(&d->intr, (uint64_t)g_report[i], 8, (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);  // on redemande un rapport
            doorbell(d->slot, d->dci);
            return;
        }
    }
    g_xfer_code = code;                                                // sinon : fin d'un transfert de contrôle
    g_xfer_done = 1;
}

// Lit tous les événements prêts : complétion de commande, fin de transfert...
static void pump_events(void) {
    int any = 0;
    for (;;) {
        volatile Trb *e = &g_ev_ring[g_ev_deq];
        uint32_t ctl = e->control;
        if ((ctl & 1) != g_ev_ccs) break;                 // le cycle ne correspond pas : plus rien de neuf
        __asm__ volatile("dmb ld" ::: "memory");
        uint32_t type = (ctl >> 10) & 0x3F, st = e->status;
        if (type == TRB_EV_CMD) {
            g_cmd_code = st >> 24; g_cmd_slot = ctl >> 24; g_cmd_done = 1;
        } else if (type == TRB_EV_TRANSFER) {
            on_transfer_event(ctl >> 24, (ctl >> 16) & 0x1F, st >> 24);
        }                                                 // autres (changement de port...) : ignorés
        if (++g_ev_deq == EV_N) { g_ev_deq = 0; g_ev_ccs ^= 1; }
        any = 1;
    }
    if (any) w64(g_rt, 0x38, (uint64_t)&g_ev_ring[g_ev_deq] | 8);   // ERDP : nouvelle position (bit 3 = acquitte)
}

void xhci_poll(void) { if (g_cap) pump_events(); }

// ---- Commandes et transferts de contrôle ---------------------------------------
// Envoie une commande ; renvoie le code de complétion (1 = succès) ou -1 si timeout
static int command(uint64_t param, uint32_t control, uint8_t *slot_out) {
    g_cmd_done = 0;
    ring_push(&g_cmd, param, 0, control);
    doorbell(0, 0);
    uint64_t t = timer_ms();
    while (!g_cmd_done) { pump_events(); if (timer_ms() - t > 1000) return -1; }
    if (slot_out) *slot_out = (uint8_t)g_cmd_slot;
    return g_cmd_code;
}

// Transfert de contrôle sur EP0 : Setup -> (Data) -> Status. Renvoie 0 si OK
static int control_xfer(Dev *d, uint8_t reqtype, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *buf) {
    int in = reqtype & 0x80;
    uint64_t setup = reqtype | ((uint64_t)req << 8) | ((uint64_t)val << 16) | ((uint64_t)idx << 32) | ((uint64_t)len << 48);
    uint32_t trt = len ? (in ? 3 : 2) : 0;                // 0 = sans données, 2 = OUT, 3 = IN
    ring_push(&d->ep0, setup, 8, (TRB_SETUP << 10) | TRB_IDT | (trt << 16));
    if (len) ring_push(&d->ep0, (uint64_t)buf, len, (TRB_DATA << 10) | (in ? TRB_DIR : 0));
    ring_push(&d->ep0, 0, 0, (TRB_STATUS << 10) | TRB_IOC | ((len && in) ? 0 : TRB_DIR));  // statut dans le sens inverse des données
    g_xfer_done = 0;
    doorbell(d->slot, 1);                                 // DCI 1 = EP0
    uint64_t t = timer_ms();
    while (!g_xfer_done) { pump_events(); if (timer_ms() - t > 1000) return -1; }
    return (g_xfer_code == 1 || g_xfer_code == 13) ? 0 : -1;
}

// ---- Contextes -------------------------------------------------------------------
// Contexte n d'une zone : 0 = contrôle (entrée seulement), 1 = slot, 2 = EP0, 3+ = autres endpoints
static uint32_t *ctx(uint8_t *base, uint32_t i) { return (uint32_t *)(base + i * g_csz); }

static uint32_t default_mps(uint8_t speed) {              // taille de paquet initiale de EP0 selon la vitesse
    switch (speed) { case 4: return 512; case 3: return 64; default: return 8; }   // SS / HS / FS+LS
}

// Remplit slot + EP0 du contexte d'entrée
static void fill_slot_ep0(Dev *d, uint8_t *in, uint32_t mps, uint32_t entries) {
    uint32_t *s = ctx(in, 1);
    s[0] = ((uint32_t)d->speed << 20) | (entries << 27);          // vitesse + nombre de contextes valides
    s[1] = ((uint32_t)(d->port + 1) << 16);                       // numéro du port racine (base 1)
    uint32_t *e = ctx(in, 2);
    e[1] = (3 << 1) | (4 << 3) | (mps << 16);                     // CErr=3, type=Control, taille de paquet
    uint64_t deq = (uint64_t)d->ep0.trbs | 1;                     // adresse de l'anneau + bit de cycle
    e[2] = (uint32_t)deq; e[3] = (uint32_t)(deq >> 32);
    e[4] = 8;                                                     // longueur moyenne de TRB
}

// ---- Énumération d'un appareil -------------------------------------------------------
// Cherche dans le descripteur de configuration une interface clavier boot (classe 3, sous-classe 1, protocole 1)
// et son endpoint "interruption IN". Renvoie 1 si trouvé.
static int find_keyboard(const uint8_t *c, int len, uint8_t *cfgval, uint8_t *iface, uint8_t *ep, uint16_t *mps, uint8_t *interval) {
    int kbd = 0;
    *cfgval = c[5];
    for (int p = 0; p + 2 <= len && c[p] != 0; p += c[p]) {
        if (c[p + 1] == 4 && p + 9 <= len) {                         // descripteur d'interface
            *iface = c[p + 2];
            kbd = (c[p + 5] == 3 && c[p + 6] == 1 && c[p + 7] == 1);
        } else if (c[p + 1] == 5 && kbd && p + 7 <= len) {           // descripteur d'endpoint
            if ((c[p + 2] & 0x80) && (c[p + 3] & 3) == 3) {          // IN + type interruption
                *ep = c[p + 2]; *mps = c[p + 4] | (c[p + 5] << 8); *interval = c[p + 6];
                return 1;
            }
        }
    }
    return 0;
}

// Exposant d'intervalle xHCI (unités de 125 µs, 2^n) à partir du bInterval du descripteur
static uint32_t interval_exp(uint8_t speed, uint8_t b) {
    uint32_t n;
    if (speed >= 3) n = b ? b - 1 : 0;                    // HS/SS : bInterval = n+1 déjà en puissance de 2
    else { uint32_t v = (b ? b : 1) * 8; n = 0; while ((2u << n) <= v) n++; }   // FS/LS : ms -> 125 µs, log2
    return n > 15 ? 15 : n;
}

// Retourne 1 si un clavier a été configuré sur ce port
static int enumerate_port(uint32_t port) {
    uint32_t off = 0x400 + 0x10 * port;                   // registre PORTSC de ce port
    uint32_t sc = r32(g_op, off);
    if (!(sc & 1)) return 0;                              // CCS : rien de branché
    const uint32_t keep = ~0x00FE0002u;                   // on masque les bits "écrire 1 pour effacer" (dont PED)

    w32(g_op, off, (sc & keep) | (1 << 4));               // PR : reset du port
    if (!wait_reg(g_op, off, 1u << 21, 1u << 21, 500)) return 0;   // PRC : reset terminé
    w32(g_op, off, (r32(g_op, off) & keep) | (1 << 21));  // efface PRC
    sc = r32(g_op, off);
    if (!(sc & 2)) return 0;                              // PED : port activé ?
    uint8_t speed = (sc >> 10) & 0xF;                     // 1=FS 2=LS 3=HS 4=SS

    int di = -1;
    for (int i = 0; i < MAX_DEV; i++) if (!g_dev[i].used) { di = i; break; }
    if (di < 0) return 0;
    Dev *d = &g_dev[di];
    memset(d, 0, sizeof(*d));
    d->port = port; d->speed = speed;

    uint8_t slot;
    if (command(0, TRB_ENABLE_SLOT << 10, &slot) != 1 || !slot) return 0;
    d->slot = slot;

    // Contexte appareil (sortie) référencé par la table DCBAA
    memset(g_devctx[di], 0, sizeof(g_devctx[di]));
    g_dcbaa[slot] = (uint64_t)g_devctx[di];
    ring_init(&d->ep0, g_ep0_ring[di], EP_N);

    // Address Device : donne une adresse USB à l'appareil
    uint8_t *in = g_inctx[di];
    memset(in, 0, sizeof(g_inctx[di]));
    ctx(in, 0)[1] = 3;                                    // flags "add" : slot (A0) + EP0 (A1)
    fill_slot_ep0(d, in, default_mps(speed), 1);
    if (command((uint64_t)in, (TRB_ADDR_DEV << 10) | ((uint32_t)slot << 24), 0) != 1) return 0;
    d->used = 1;

    // Taille réelle de paquet EP0 : 8 premiers octets du descripteur d'appareil, octet 7
    memset(g_desc, 0, 64);
    if (control_xfer(d, 0x80, 6, 0x0100, 0, 8, g_desc) != 0) return 0;
    uint32_t mps0 = (speed == 4) ? (1u << g_desc[7]) : g_desc[7];   // SS : 2^n ; sinon valeur directe
    if (mps0 && mps0 != default_mps(speed)) {             // différente de notre hypothèse : Evaluate Context
        memset(in, 0, sizeof(g_inctx[di]));
        ctx(in, 0)[1] = 2;                                // A1 seulement
        fill_slot_ep0(d, in, mps0, 1);
        command((uint64_t)in, (TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24), 0);
    }

    // Descripteur de configuration : d'abord 9 octets pour connaître la taille totale
    memset(g_desc, 0, sizeof(g_desc));
    if (control_xfer(d, 0x80, 6, 0x0200, 0, 9, g_desc) != 0) return 0;
    uint16_t total = g_desc[2] | (g_desc[3] << 8);
    if (total > sizeof(g_desc)) total = sizeof(g_desc);
    if (control_xfer(d, 0x80, 6, 0x0200, 0, total, g_desc) != 0) return 0;

    uint8_t cfgval, iface, ep, interval; uint16_t mps;
    if (!find_keyboard(g_desc, total, &cfgval, &iface, &ep, &mps, &interval)) { d->used = 0; return 0; }  // pas un clavier

    if (control_xfer(d, 0x00, 9, cfgval, 0, 0, 0) != 0) return 0;      // SET_CONFIGURATION
    control_xfer(d, 0x21, 0x0B, 0, iface, 0, 0);                        // SET_PROTOCOL : 0 = boot (rapports de 8 octets)
    control_xfer(d, 0x21, 0x0A, 0, iface, 0, 0);                        // SET_IDLE : pas de rapports répétés inutiles

    // Configure Endpoint : active l'endpoint interruption IN du clavier
    uint32_t dci = (ep & 0xF) * 2 + 1;                                  // numéro de contexte de l'endpoint (IN)
    ring_init(&d->intr, g_int_ring[di], EP_N);
    memset(in, 0, sizeof(g_inctx[di]));
    ctx(in, 0)[1] = 1 | (1u << dci);                                    // A0 (slot) + endpoint
    uint32_t *s = ctx(in, 1);
    s[0] = ((uint32_t)speed << 20) | (dci << 27);
    s[1] = ((uint32_t)(port + 1) << 16);
    uint32_t *e = ctx(in, dci + 1);
    e[0] = interval_exp(speed, interval) << 16;
    e[1] = (3 << 1) | (7 << 3) | ((uint32_t)mps << 16);                 // CErr=3, type=Interrupt IN
    uint64_t deq = (uint64_t)d->intr.trbs | 1;
    e[2] = (uint32_t)deq; e[3] = (uint32_t)(deq >> 32);
    e[4] = 8 | ((uint32_t)mps << 16);                                   // longueur moyenne + charge max par intervalle
    if (command((uint64_t)in, (TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24), 0) != 1) return 0;

    // Lance la lecture : un TRB "normal" qui recevra le rapport clavier
    d->dci = (uint8_t)dci;
    memset(g_report[di], 0, 8);
    ring_push(&d->intr, (uint64_t)g_report[di], 8, (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
    doorbell(slot, dci);
    return 1;
}

// ---- Initialisation du contrôleur ----------------------------------------------------
int xhci_init(uint64_t base) {
    g_cap = (volatile uint8_t *)base;
    uint32_t hcs1 = r32(g_cap, 4), hcs2 = r32(g_cap, 8), hcc = r32(g_cap, 0x10);
    g_op = g_cap + (g_cap[0] & 0xFF);                     // CAPLENGTH : début des registres opérationnels
    g_db = g_cap + (r32(g_cap, 0x14) & ~3u);              // DBOFF : doorbells
    g_rt = g_cap + (r32(g_cap, 0x18) & ~0x1Fu);           // RTSOFF : registres runtime
    uint32_t max_slots = hcs1 & 0xFF, max_ports = hcs1 >> 24;
    g_csz = (hcc & 4) ? 64 : 32;

    // 1. Arrêt puis reset complet (le firmware a pu laisser le contrôleur dans un état quelconque)
    w32(g_op, 0, r32(g_op, 0) & ~1u);                     // USBCMD.RS = 0
    if (!wait_reg(g_op, 4, 1, 1, 200)) { kprintf("xHCI: arret impossible\n"); return 0; }   // USBSTS.HCH = 1
    w32(g_op, 0, 2);                                      // USBCMD.HCRST
    if (!wait_reg(g_op, 0, 2, 0, 500) || !wait_reg(g_op, 4, 1u << 11, 0, 500)) { kprintf("xHCI: reset impossible\n"); return 0; }

    // 2. Slots, scratchpad, DCBAA
    w32(g_op, 0x38, max_slots);                           // CONFIG : tous les slots
    uint32_t nsp = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);   // pages "scratchpad" exigées par le contrôleur
    if (nsp > MAX_SCRATCH) { kprintf("xHCI: %u pages scratchpad (max %u)\n", nsp, MAX_SCRATCH); return 0; }
    memset(g_dcbaa, 0, sizeof(g_dcbaa));
    for (uint32_t i = 0; i < nsp; i++) g_scratch_arr[i] = (uint64_t)g_scratch_pg[i];
    if (nsp) g_dcbaa[0] = (uint64_t)g_scratch_arr;
    w64(g_op, 0x30, (uint64_t)g_dcbaa);                   // DCBAAP

    // 3. Anneau de commandes (CRCR : adresse + bit RCS = 1)
    ring_init(&g_cmd, g_cmd_ring, CMD_N);
    w64(g_op, 0x18, (uint64_t)g_cmd_ring | 1);

    // 4. Anneau d'événements (interrupteur 0) : ERSTSZ, ERDP puis ERSTBA (dans cet ordre)
    memset(g_ev_ring, 0, sizeof(g_ev_ring));
    g_ev_deq = 0; g_ev_ccs = 1;
    g_erst.base = (uint64_t)g_ev_ring; g_erst.size = EV_N; g_erst.rsvd = 0;
    DSB();
    w32(g_rt, 0x28, 1);
    w64(g_rt, 0x38, (uint64_t)g_ev_ring);
    w64(g_rt, 0x30, (uint64_t)&g_erst);

    // 5. Démarrage (sans interruptions : on scrute l'anneau d'événements)
    w32(g_op, 0, r32(g_op, 0) | 1);                       // USBCMD.RS = 1
    if (!wait_reg(g_op, 4, 1, 0, 200)) { kprintf("xHCI: demarrage impossible\n"); return 0; }

    // 6. Laisse le temps aux appareils d'être détectés, puis énumère les ports
    uint64_t t = timer_ms();
    while (timer_ms() - t < 200) {}
    int kbds = 0;
    for (uint32_t p = 0; p < max_ports; p++) kbds += enumerate_port(p);
    return kbds;
}
