; SSI DMA c2p: the DSP converts a chunky 8-bit screen the sound DMA streams
; through it into Falcon 8-plane words, which DMA record writes back.
;
; The route is the pass-through's (dsp/ssiecho.asm): four stereo tracks each
; way, all eight 16-bit slots of the crossbar frame carrying data. Eight
; slots are eight words, sixteen chunky pixels in and eight plane words out,
; so one crossbar frame is exactly one group.
;
; I/O. The SSI receive interrupt is a fast interrupt, two instructions: it
; stores the received word at X:(r7) and transmits Y:(r7), then advances r7
; through a 32-word ring (X:$40-$5f in, Y:$40-$5f out). Transmit therefore
; runs in lockstep with receive, one word per slot, and each output word
; leaves 32 slots after the input word in the same ring position arrived:
; the main loop converts a group into the output ring positions of its own
; input, and has until those come round again, three groups later, to do so.
; On average it must keep up with one group per eight slots. r7, m7 and r6
; (the receive overrun count) belong to the interrupts.
;
; Framing. The host starts each screen with a marker, four words
; ($a55a, $5aa5, $c33c, $3cc3), and the word after it starts group 0. After
; CMD_SYNC the main loop scans for the marker; once found it converts every
; following group, whatever the host sends, until the next CMD_SYNC. The
; host pads the screen with at least 32 words after it, so the last groups'
; output is clocked out.
;
; Timing. Hatari counts 190 instruction cycles for a group, the conversion
; (C2PCORE.INC, from gen-c2p.py) and the loop around it, and 4 for each
; slot's interrupt: about 222 of the 325 cycles a group lasts at 49,170 Hz
; frames, and of the 256 at 62,500 Hz. The main loop records the deepest
; backlog it found at the start of a group; more than 24 words means the
; ring nearly overran.
;
; The program is a standalone Dsp_ExecBoot image: P memory only, below
; P:$0200.
;
; Keep the command words in sync with m68k/ssic2p.s.

        include 'ioequ.inc'

CMD_PING        equ     $010000
CMD_SYNC        equ     $020000         ; scan for the marker again
CMD_GROUPS      equ     $030000         ; reply: groups converted since sync
CMD_BACKLOG     equ     $040000         ; reply: deepest backlog, in words
CMD_OVERRUNS    equ     $050000         ; reply: receive overruns
CMD_STATE       equ     $060000         ; reply: 0 scanning, 1 converting
CMD_ARM         equ     $070000         ; low byte: slots per frame - 1
CMD_PROBE       equ     $080000         ; low byte 0: r7, 1: SSI SR, 2: SR, 3: r0
REPLY_PING      equ     $433250         ; "C2P"
REPLY_ERROR     equ     $ffffff

MARK0           equ     $a55a00
MARK1           equ     $5aa500
MARK2           equ     $c33c00
MARK3           equ     $3cc300

; Internal X. Equates only: a ds block would emit an empty X section that the
; boot-image converter rejects.
GROUPS          equ     $0000
BACKLOG         equ     $0001
STATE           equ     $0002
SR_COPY         equ     $0003
MARKS           equ     $0030           ; the four marker words
RING            equ     $0040           ; 32 words, X in and Y out

; Words waiting at r0, (r7 - r0) mod 32, into a1, with Z set when none.
; AND leaves a2 alone: a wrapped difference keeps its sign there, so a
; signed comparison needs a1 moved back into a first.
AVAILABLE       macro
        move    r7,a
        move    r0,x0
        sub     x0,a
        move    #>31,x0
        and     x0,a
        endm

; ---------------------------------------------------------------------------
        org     p:$0000
        jmp     c2p_start

        org     p:$000c                 ; SSI receive data: fast interrupt
        movep   x:m_rx,x:(r7)
        movep   y:(r7)+,x:m_tx
        jsr     c2p_overrun             ; SSI receive data with exception

; ---------------------------------------------------------------------------
        org     p:$0040
c2p_start:
        movep   #1,x:m_pbc              ; host port
        movep   #$1f8,x:m_pcc           ; port C pins to the SSI: they reset to
                                        ; GPIO, which leaves the SSI clockless
        movep   #$3000,x:m_ipr          ; SSI interrupts at level 2
        move    #>MARK0,x0
        move    x0,x:MARKS
        move    #>MARK1,x0
        move    x0,x:MARKS+1
        move    #>MARK2,x0
        move    x0,x:MARKS+2
        move    #>MARK3,x0
        move    x0,x:MARKS+3
        move    #0,r6
        move    #>7,a
        jsr     c2p_arm
        andi    #$fc,mr                 ; unmask interrupts
        jmp     c2p_scan

; ---------------------------------------------------------------------------
; Scanning: follow the ring with r0 and match the marker, r3 walking it.
; ---------------------------------------------------------------------------
c2p_scan:
        clr     a
        move    a1,x:STATE
c2p_scan_loop:
        jclr    #0,x:m_hsr,c2p_scan_quiet
        jsr     c2p_host
        tst     b                       ; a command asked for a new state
        jne     c2p_scan
c2p_scan_quiet:
        AVAILABLE
        jeq     c2p_scan_loop
        move    x:(r0)+,a
        move    x:(r3)+,x0
        cmp     x0,a
        jne     c2p_scan_miss
        move    r3,b                    ; all four matched?
        move    #>MARKS+4,x0
        cmp     x0,b
        jne     c2p_scan_loop
        jmp     c2p_convert
c2p_scan_miss:
        move    #MARKS,r3               ; a miss may be the marker's start
        nop
        move    x:(r3)+,x0
        cmp     x0,a
        jeq     c2p_scan_loop
        move    #MARKS,r3
        jmp     c2p_scan_loop

; ---------------------------------------------------------------------------
; Converting: one group each time eight words wait at r0.
; ---------------------------------------------------------------------------
c2p_convert:
        clr     a
        move    a1,x:GROUPS
        move    a1,x:BACKLOG
        move    #>1,a
        move    a1,x:STATE
c2p_wait:
        jclr    #0,x:m_hsr,c2p_wait_quiet
        jsr     c2p_host
        tst     b
        jne     c2p_scan
c2p_wait_quiet:
        AVAILABLE
        move    a1,a
        move    #>8,x0
        cmp     x0,a
        jlt     c2p_wait
        move    x:BACKLOG,x0            ; the deepest backlog so far
        cmp     x0,a
        jle     c2p_backlog_kept
        move    a1,x:BACKLOG
c2p_backlog_kept:
        ; r1/r2 at input words 3 and 7, r4/r5 at output words 0 and 1
        move    r0,r1
        move    r0,r2
        move    r0,r4
        move    r0,r5
        move    (r1)+n1                 ; n1 = 3
        move    (r2)+n2                 ; n2 = 7
        move    (r5)+
        nop
        include 'C2PCORE.INC'
        move    (r0)+n0                 ; n0 = 8: the next group
        move    x:GROUPS,a
        move    #>1,x0
        add     x0,a
        move    a1,x:GROUPS
        jmp     c2p_wait


; ---------------------------------------------------------------------------
; Host commands, one word each, answered with one word; called once HRDF is
; set. b is 0 on return unless the command changed the state (sync or arm):
; then the caller restarts scanning. The reply is written without waiting for HTDE: the host
; reads every reply before it sends the next command.
; ---------------------------------------------------------------------------
c2p_host:
        movep   x:m_hrx,y1              ; the whole command word
        move    y1,b
        move    #>$ff0000,x0
        and     x0,b
        move    #>CMD_PING,x0
        cmp     x0,b
        jeq     c2p_cmd_ping
        move    #>CMD_SYNC,x0
        cmp     x0,b
        jeq     c2p_cmd_sync
        move    #>CMD_GROUPS,x0
        cmp     x0,b
        jeq     c2p_cmd_groups
        move    #>CMD_BACKLOG,x0
        cmp     x0,b
        jeq     c2p_cmd_backlog
        move    #>CMD_OVERRUNS,x0
        cmp     x0,b
        jeq     c2p_cmd_overruns
        move    #>CMD_STATE,x0
        cmp     x0,b
        jeq     c2p_cmd_state
        move    #>CMD_ARM,x0
        cmp     x0,b
        jeq     c2p_cmd_arm
        move    #>CMD_PROBE,x0
        cmp     x0,b
        jeq     c2p_cmd_probe
        movep   #REPLY_ERROR,x:m_htx
        jmp     c2p_host_quiet
c2p_cmd_ping:
        movep   #REPLY_PING,x:m_htx
        jmp     c2p_host_quiet
c2p_cmd_groups:
        movep   x:GROUPS,x:m_htx
        jmp     c2p_host_quiet
c2p_cmd_backlog:
        movep   x:BACKLOG,x:m_htx
        jmp     c2p_host_quiet
c2p_cmd_state:
        movep   x:STATE,x:m_htx
        jmp     c2p_host_quiet
c2p_cmd_overruns:
        move    r6,x0
        movep   x0,x:m_htx
        jmp     c2p_host_quiet
c2p_cmd_probe:
        move    y1,a
        move    #>$ff,x0
        and     x0,a
        move    r7,x0
        tst     a
        jeq     c2p_probe_reply
        movep   x:m_sr,x0
        move    #>1,y0
        cmp     y0,a
        jeq     c2p_probe_reply
        move    sr,x0
        move    #>2,y0
        cmp     y0,a
        jeq     c2p_probe_reply
        move    r0,x0
c2p_probe_reply:
        movep   x0,x:m_htx
        jmp     c2p_host_quiet
c2p_cmd_arm:
        move    y1,a
        move    #>$ff,x0
        and     x0,a
        jsr     c2p_arm
c2p_cmd_sync:
        move    r7,r0                   ; skip what arrived before
        move    #MARKS,r3
        movep   #0,x:m_htx
        move    #>1,b                   ; restart scanning
        rts
c2p_host_quiet:
        clr     b
        rts

; ---------------------------------------------------------------------------
; Program the SSI from scratch and reset the rings: 16-bit words, a1+1 slots
; per network frame, external clock and frame sync, receiver (with its
; interrupt) and transmitter on. The host arms it again once Devconnect has
; given the crossbar clock, as the production kernel only arms after the
; route exists. in: a1 = slots per frame - 1   clobbers a, x0, r0
; ---------------------------------------------------------------------------
c2p_arm:
        ori     #$03,mr                 ; no receive interrupt while arming
        movep   #0,x:m_crb
        rep     #8
        lsl     a
        move    #>$4000,x0              ; WL = 16 bits
        or      x0,a
        movep   a1,x:m_cra
        move    #RING,r0
        clr     a
        do      #32,c2p_arm_cleared
        move    a1,x:(r0)
        move    a1,y:(r0)+
c2p_arm_cleared:
        move    #RING,r7
        move    #31,m7
        move    #31,m0
        move    #31,m1
        move    #31,m2
        move    #31,m4
        move    #31,m5
        move    #>3,n1                  ; the constant offsets of the group loop
        move    #>7,n2
        move    #>2,n4
        move    #>2,n5
        move    #>8,n0
        move    #RING,r0
        move    #MARKS,r3
        movep   #0,x:m_tx
        movep   #$ba00,x:m_crb          ; RIE, RE, TE, network, synchronous
        andi    #$fc,mr
        rts

; Receive overrun: the exception vector, taken while ROE is set. Reading the
; status register and then the receive register clears it; the word is
; kept, so the ring stays in step, and r6 counts the event.
c2p_overrun:
        movep   x:m_sr,x:SR_COPY
        movep   x:m_rx,x:(r7)
        movep   y:(r7)+,x:m_tx
        move    (r6)+
        rti

        end
