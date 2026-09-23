; SSI DMA pass-through: the DSP half of the DMA c2p feasibility test.
;
; DMA playback feeds the SSI receiver through the crossbar and the SSI
; transmitter feeds DMA record, both in the continuous 128-bit frame of
; eight 16-bit slots. This program echoes every received word into the
; transmitter unchanged, so the host can check that a buffer comes back
; intact and in order, and time how fast it does.
;
; The echo is a plain FIFO: the word received in one slot goes out a slot
; or two later, whatever the hardware's TX load timing is. The host
; therefore records all eight slots (four stereo tracks) and finds the
; offset itself; slot alignment is left to the c2p program proper.
;
; Three counters run beside the echo:
;   a1  received words
;   a0  frame syncs (RFS, set for slot 0's word)
;   b1  the sum of slot phase errors: r0 counts slots modulo eight, and at
;       every frame sync its value, which is 0 unless a slot was missed or
;       doubled, is added here before r0 restarts. Any lost word shows.
; The per-word path is 22 instruction cycles and the frame sync adds 12; a
; slot lasts 40 cycles at 49,170 Hz frames and 32 at 62,500 Hz (the 32 MHz
; clock), and RX holds one word while the next shifts in.
;
; Host commands are one word each and answered with one word. They are
; taken only while no received word is pending, and the reply is written
; without waiting for HTDE: the host reads every reply before it sends the
; next command, and a wait here could cost received words. Every command
; parks the counters at once, so CMD_SNAPSHOT's copy of the three is taken
; between two words, and the two reads that follow it return that copy.
;
; The program is a standalone Dsp_ExecBoot image: P memory only, below
; P:$0200, no interrupts.
;
; Keep the command words in sync with m68k/ssiecho.s.

        include 'ioequ.inc'

CMD_PING        equ     $010000
CMD_SNAPSHOT    equ     $020000         ; copy the counters; reply: words
CMD_READ_FRAMES equ     $030000         ; reply: the copy's frame syncs
CMD_READ_PHASE  equ     $040000         ; reply: the copy's phase error sum
CMD_RESET       equ     $050000
CMD_ARM         equ     $060000         ; low byte: slots per frame - 1
CMD_READ_SR     equ     $070000
REPLY_PING      equ     $454348         ; "ECH"
REPLY_ERROR     equ     $ffffff

; Internal X scratch, named by equates: a ds block would emit an empty X
; section that the boot-image converter rejects.
SAVE_WORDS      equ     $0000
SAVE_FRAMES     equ     $0001
SAVE_PHASE      equ     $0002
SNAP_WORDS      equ     $0003
SNAP_FRAMES     equ     $0004
SNAP_PHASE      equ     $0005

        org     p:$0000
        jmp     echo_start

        org     p:$0040
echo_start:
        movep   #1,x:m_pbc              ; host port
        movep   #$1f8,x:m_pcc           ; port C pins to the SSI: they reset to
                                        ; GPIO, which leaves the SSI clockless
        move    #>1,x1                  ; word increment, into a1
        move    #0,y1                   ; y1:y0 = 1: frame increment, into a0
        move    #>1,y0
        move    #0,r0                   ; slot phase, modulo eight
        move    #7,m0
        move    #>7,a                   ; eight slots until the host arms
        jsr     echo_arm
        clr     a
        clr     b

; The hot path. RFS is tested before RX is read: it describes the word RX
; holds.
echo_loop:
        jclr    #7,x:m_sr,echo_host     ; RDF: a word arrived
        jset    #3,x:m_sr,echo_frame    ; RFS: it is slot 0's
echo_word:
        movep   x:m_rx,x0
        movep   x0,x:m_tx
        add     x1,a    (r0)+
        jmp     echo_loop
echo_frame:
        add     y,a                     ; frames, in a0
        move    r0,x0                   ; 0 when no slot went astray
        add     x0,b
        move    #0,r0
        jmp     echo_word

; The slow path: the counters are parked while a and b serve the dispatch.
echo_host:
        jclr    #0,x:m_hsr,echo_loop    ; HRDF: a command arrived
        move    a1,x:SAVE_WORDS
        move    a0,x:SAVE_FRAMES
        move    b1,x:SAVE_PHASE
        movep   x:m_hrx,b
        move    b1,y1                   ; the whole command word
        move    #>$ff0000,x0
        and     x0,b
        move    #>CMD_PING,x0
        cmp     x0,b
        jeq     echo_cmd_ping
        move    #>CMD_SNAPSHOT,x0
        cmp     x0,b
        jeq     echo_cmd_snapshot
        move    #>CMD_READ_FRAMES,x0
        cmp     x0,b
        jeq     echo_cmd_frames
        move    #>CMD_READ_PHASE,x0
        cmp     x0,b
        jeq     echo_cmd_phase
        move    #>CMD_RESET,x0
        cmp     x0,b
        jeq     echo_cmd_reset
        move    #>CMD_ARM,x0
        cmp     x0,b
        jeq     echo_cmd_arm
        move    #>CMD_READ_SR,x0
        cmp     x0,b
        jeq     echo_cmd_sr
        movep   #REPLY_ERROR,x:m_htx
        jmp     echo_resume

echo_cmd_ping:
        movep   #REPLY_PING,x:m_htx
        jmp     echo_resume
echo_cmd_snapshot:
        move    x:SAVE_WORDS,x0
        move    x0,x:SNAP_WORDS
        move    x:SAVE_FRAMES,x0
        move    x0,x:SNAP_FRAMES
        move    x:SAVE_PHASE,x0
        move    x0,x:SNAP_PHASE
        movep   x:SNAP_WORDS,x:m_htx
        jmp     echo_resume
echo_cmd_frames:
        movep   x:SNAP_FRAMES,x:m_htx
        jmp     echo_resume
echo_cmd_phase:
        movep   x:SNAP_PHASE,x:m_htx
        jmp     echo_resume
echo_cmd_sr:
        movep   x:m_sr,x:m_htx
        jmp     echo_resume
echo_cmd_arm:
        move    #>$ff,x0
        move    y1,a
        and     x0,a
        jsr     echo_arm
echo_cmd_reset:
        clr     a
        move    a1,x:SAVE_WORDS
        move    a1,x:SAVE_FRAMES
        move    a1,x:SAVE_PHASE
        movep   a1,x:m_htx

echo_resume:
        move    x:SAVE_WORDS,a
        move    x:SAVE_FRAMES,a0
        move    x:SAVE_PHASE,b
        move    #0,y1                   ; the frame increment again
        jmp     echo_loop

; Program the SSI from scratch: 16-bit words, a1+1 slots per network frame,
; external clock and frame sync, receiver and transmitter on, polled. The
; host arms it again once Devconnect has given the crossbar clock, as the
; production kernel only arms after the route exists.
; in: a1 = slots per frame - 1   clobbers a, x0
echo_arm:
        movep   #0,x:m_crb
        rep     #8
        lsl     a
        move    #>$4000,x0              ; WL = 16 bits
        or      x0,a
        movep   a1,x:m_cra
        movep   #$3a00,x:m_crb          ; RE, TE, network, synchronous
        movep   #0,x:m_tx
        rts

        end
