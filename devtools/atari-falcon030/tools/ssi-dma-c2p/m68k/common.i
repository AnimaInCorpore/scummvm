; Shared equates and macros of the SSI DMA tests (ssiecho.s, ssic2p.s).

; Sound matrix. Devconnect sources and destinations, clocks, protocol.
SRC_DMAPLAY     equ     0
SRC_DSPXMIT     equ     1
DST_DMAREC      equ     1
DST_DSPRECV     equ     2
DST_DAC         equ     8
CLK_25M         equ     0
CLK_32M         equ     2
NO_SHAKE        equ     1
SOUND_STEREO16  equ     1
SOUND_ADDERIN   equ     4
SOUND_MATRIXIN  equ     2
SNDSTAT_RESET   equ     1
TRACKS4         equ     3                 ; Settracks counts from 0
BUF_PLAY        equ     0
BUF_RECORD      equ     1
OP_PLAY         equ     1
OP_PLAY_REPEAT  equ     2
OP_RECORD       equ     4
OP_RECORD_REPEAT equ    8

; Hardware registers.
SND_CONTROL     equ     $ffff8901         ; bit 0 play, bit 4 record enable
HOST_ISR        equ     $ffffa202         ; bit 0 RXDF, bit 1 TXDE
HOST_DATA       equ     $ffffa204         ; a long covers the three bytes
MFP_TCDR        equ     $fffffa23         ; Timer C data: counts 192 to 1
TIMER_RELOAD    equ     192
FINE_HZ         equ     38400             ; Timer C counts per second
HZ200           equ     $4ba

SETTLE_TICKS    equ     40                ; 200 ms after connecting (TOS tick)

; Append the NUL-terminated fragment to the line being built at (a0)+.
        macro   FSTR fragment
        lea     \1,a1
        bsr     fmt_string
        endm

; Current play and record DMA addresses into four longs at the argument.
        macro   Buffptr pointers
        pea     \1
        move.w  #141,-(sp)
        trap    #14
        addq.l  #6,sp
        endm

; Timer C, polled: d6 counts the counter's wraps (5 ms each) and d5.b holds
; its last value. Poll more often than every 5 ms. Clobbers d2 and d4.
;
; Each poll first spins through POLL_SPIN short instructions: about 0.4 ms
; on a Falcon with its caches on, about 1.3 ms under Hatari, well inside the
; 5 ms wrap either way. Device reads are slow, and Hatari lets the DSP run
; only between 68030 instructions, so every MFP or sound register read there
; risks two SSI words arriving with no DSP cycles between them; the spin
; keeps such reads to a few hundred a second.
POLL_SPIN       equ     1000
        macro   TIMER_POLL
        move.w  #POLL_SPIN-1,d4
.s\@:
        dbf     d4,.s\@
        move.b  MFP_TCDR.w,d2
        cmp.b   d5,d2
        bls     .\@
        addq.l  #1,d6
.\@:
        move.b  d2,d5
        endm
