; Practical (block-rate) two-operator OPL kernel for the Falcon's DSP56001.
;
; The DSP side of opl-practical.h: a memory-image machine whose operator and
; channel records the 68030 writes, and whose output must match that host
; reference word for word. It renders 64-frame blocks at the codec rate,
; operator-major: each operator runs one hardware loop over the block, the
; modulator writing an internal ring the carrier consumes. Envelopes and the
; LFO advance once per block, at the boundary, from tables the host uploads.
;
; This file is the bench/exactness build: it owns no SSI and no interrupt,
; and takes its events from a table the host uploads. The production
; transport adds a period-paced host protocol on top of render_block.
;
; Memory map (words)
;   P internal $0080-$01ff  stages, render driver, per-operator boundary pass
;   P external $2000-       start, command loop, per-block and per-channel code
;   X internal $0000-$003f  scalars ($10-$13 host-written)
;   X internal $0040-$007f  modulation ring, 64 words
;   X internal $0080-$0091  feedback history, newer product, 18 channels
;   Y internal $0000-$003f  mix ring, 64 words
;   Y internal $0040-$0063  gain pairs of feedback modulators, [history, onward]
;   Y internal $0080-$0091  feedback history, older product
;   X external $0200-$03ff  gain table 2^(-envOut/32) / 2, 512 words
;   X external $0400-$087f  operator records, 32 words each, 36 operators
;   X external $0900-$09ff  channel records, 8 words each
;   X external $0a00-$0a3f  attack factor per block, by 6-bit rate
;   X external $0a40-$0a7f  decay step per block, by rate
;   X external $0a80-$0a87  record offset of the vibrato delta, by LFO position
;   X external $0b00-$0b8f  per-operator render parameters, 4 words each
;   X external $2000-$2fff  bench output, 128 blocks
;   X external $3000-$3fff  bench events, (block << 16 | address), value
;   X external $2000-$3fff  stream events, same format, 4,096 of them
;   Y external $0400-$087f  phase fractions, the L-space partners of the records
;   Y external $1000-$1fff  waveforms as linear samples * 256, 1,024 each
;
; Arithmetic conventions shared with the host reference
;   phase      48-bit index.fraction in B; the index is B1 masked to 10 bits
;   advance    mac x1,y1,b with y1 = $3ff: the increment word times 2046
;   product    mpy of a sample by a 24-bit fractional gain, A1 truncated
;   mix        24-bit ring sums stored with the accumulator limiter
;   output     the mix times the master gain, rounded, then doubled and
;              limited; the 16-bit sample is the top

        include 'ioequ.inc'
        include 'oplrttab.inc'          ; the generated block, period and LFO constants (8.3 for the DOS assembler)

; ---------------------------------------------------------------- layout

GAIN_TABLE      equ     $0200
OP_BASE         equ     $0400
OP_STRIDE       equ     32
CH_BASE         equ     $0900
CH_STRIDE       equ     8
ATTACK_TABLE    equ     $0a00
DECAY_TABLE     equ     $0a40
VIB_TABLE       equ     $0a80
RT_BASE         equ     $0b00
RT_STRIDE       equ     4
FRAME_BASE      equ     $2000
EVENT_BASE      equ     $3000
STREAM_EVENT_BASE equ   $2000           ; stream mode: the bench output area, 4,096 events

PCM_STAGE       equ     $0c00           ; expanded PCM of one period, 768 words
SSI_RING        equ     $1000           ; two periods of interleaved stereo
BLOCK_FRAMES    equ     OPL_BLOCK_FRAMES ; 64: 1.30 ms at the codec's 49,170 Hz
PERIOD_BLOCKS   equ     OPL_PERIOD_BLOCKS ; 12: 768 frames, 15.62 ms
SSI_HALF_WORDS  equ     2*BLOCK_FRAMES*PERIOD_BLOCKS
SSI_RING_WORDS  equ     2*SSI_HALF_WORDS
PCM_PER_PERIOD  equ     OPL_PCM_PER_PERIOD ; 192: host PCM at a quarter of the codec rate

MOD_RING        equ     $0040           ; X internal, BLOCK_FRAMES words
HIST_BASE       equ     $0080           ; X and Y internal
MIX_RING        equ     $0000           ; Y internal, BLOCK_FRAMES words
GAIN_RING       equ     $0040           ; Y internal, two words per channel

; operator record offsets, in the order the boundary pass walks them
OPR_PHASE       equ     0
OPR_TRIG        equ     1
OPR_TRIGSEEN    equ     2
OPR_FLAGS       equ     3
OPR_STATE       equ     4
OPR_ENV         equ     5
OPR_SL          equ     6
OPR_RATE_A      equ     7
OPR_TLKSL       equ     11
OPR_INCBASE     equ     12
OPR_WFBASE      equ     14
OPR_GAIN        equ     15
OPR_GAINMOD     equ     16
OPR_GAINFB      equ     17
OPR_INC         equ     18
OPR_VIBDELTA    equ     19              ; five words: zero, +half, +full, -half, -full

CHR_MODE        equ     0               ; render routine address
CHR_CONN        equ     1
CHR_FBMUL       equ     2

ENV_SNAP        equ     $1f8000         ; 0x1f8 << 12
ENV_SILENT      equ     $1ff000
ATTACK_DONE     equ     OPL_ATTACK_DONE ; the attack ends this close to zero

CMD_PING        equ     $01
CMD_WRITE_X     equ     $02             ; address word, count word, then the data
CMD_WRITE_Y     equ     $03
CMD_EVENTS      equ     $04             ; arg = event count in the event table
CMD_RENDER      equ     $05             ; arg = block count, at most 128
CMD_READ_X      equ     $06             ; arg = address
CMD_REWIND      equ     $07             ; output and event pointers to their bases
CMD_STOP        equ     $08             ; end of the profiled run
CMD_STREAM_START equ    $09             ; own the SSI and enter the stream loop
CMD_REFILL      equ     $0a             ; one period: events, then PCM
CMD_STREAM_STOP equ     $0b
CMD_STATUS      equ     $0c             ; reply late periods << 12 | periods rendered
CMD_CHECKSUM    equ     $0d             ; reply the running sum of emitted words
REPLY_PING      equ     $4f5052         ; "OPR"
REPLY_READY     equ     $524459         ; "RDY": the receive loop is parked
REPLY_ERROR     equ     $ffffff

; ---------------------------------------------------------------- scalars

        org     x:$0000
last_command:   ds      1
frame_pointer:  ds      1
event_read:     ds      1
event_count:    ds      1
block_index:    ds      1
tremolo_phase:  ds      1
vibrato_phase:  ds      1
tremolo_value:  ds      1
vibrato_offset: ds      1               ; this block's vibrato delta, as an offset from WFBASE
k_sixty:        ds      1               ; constants of the boundary pass, set at start:
render_ch:      ds      1
render_rt:      ds      1
render_op:      ds      1
render_hist:    ds      1
render_gain:    ds      1
k_attack_done:  ds      1               ; a short absolute load is one word, an immediate two

        org     x:$0010
tremolo_shift:  ds      1               ; host: 4 (1 dB) or 2 (4.8 dB)
fm_paused:      ds      1               ; host: freeze and silence FM, still emit PCM
channel_count:  ds      1               ; host: 9 or 18
master_gain:    ds      1               ; host: fraction applied to the FM mix
k_env_silent:   ds      1
k_env_snap:     ds      1
k_env_out_max:  ds      1
ob_gain_mod:    ds      1               ; the modulator's, kept across the carrier
ob_gainmod_mod: ds      1
ob_gainfb_mod:  ds      1
ob_gain_car:    ds      1
save_r7:        ds      1
emit_routine:   ds      1               ; emit_block (bench) or emit_block_stream
stream_next_half: ds    1               ; 0 = ring half A, 1 = half B
out_pointer:    ds      1               ; stream: next ring word to write
pcm_read:       ds      1               ; stream: next expanded PCM word
pcm_present:    ds      1
pcm_previous:   ds      1
checksum:       ds      1
periods_rendered: ds    1
late_periods:   ds      1
ssi_status:     ds      1
stream_primed:  ds      1               ; nonzero once the first period is rendered
tracking:       ds      1               ; nonzero while streaming: track_halves runs
tx_half_seen:   ds      1               ; the half the transmitter was last seen in
fresh_a:        ds      1               ; a period was rendered into half A in time
fresh_b:        ds      1               ; the same for half B
stream_live:    ds      1               ; a rendered period has played: count from here
th_a2:          ds      1               ; track_halves keeps a and x0 here
th_a1:          ds      1
th_a0:          ds      1
th_x0:          ds      1

; ------------------------------------------------------------ entry vectors

        org     p:$0000
        jmp     start

; Buffered SSI owns r6/m6 while streaming: the transmit interrupt moves one
; prepared word without touching synthesis state. The exception vector
; clears an underrun by reading the status register first.
        org     p:$0010
        movep   x:(r6)+,x:m_tx
        nop
        org     p:$0012
        jsr     ssi_tx_exception

; P:$0040-$007f stays free for the transient second-stage loader.

; =========================================================== hot code
        org     p:$0080

; ------------------------------------------------------------- stages
;
; Every stage enters with B = the operator's phase (from L:(r7)), x1 = its
; increment, y1 = $3ff, r0 = its waveform table, and leaves the advanced
; phase back in L:(r7). Gains ride in y0 (or x0 for the serial carrier,
; whose y0 is a temporary). The ring pointers are set per stage.

; An unmodulated operator writing the modulation ring: y0 = ring gain.
; Software-pipelined: the first store lands in the ring's last slot under
; a modulo of the block length, and the epilogue commits the last product.
stage_mod_plain:
        move    #MOD_RING+BLOCK_FRAMES-1,r3
        move    #BLOCK_FRAMES-1,m3
        and     y1,b
        move    b1,n0
        do      #BLOCK_FRAMES,smp_done
        mac     x1,y1,b   y:(r0+n0),x0
        and     y1,b      a,x:(r3)+
        move    b1,n0
        mpy     x0,y0,a
smp_done:
        move    a,x:(r3)+
        move    #>-1,m3
        move    b10,l:(r7)
        rts

; An unmodulated operator accumulating into the mix ring: y0 = mix gain.
stage_indep_mix:
        move    #MIX_RING,r5
        and     y1,b
        move    b1,n0
        do      #BLOCK_FRAMES,sim_done
        mac     x1,y1,b   y:(r0+n0),x0
        and     y1,b      y:(r5),a
        move    b1,n0
        mac     x0,y0,a
        move    a,y:(r5)+
sim_done:
        move    b10,l:(r7)
        rts

; A carrier modulated by the ring, accumulating into the mix: x0 = gain.
stage_serial_mix:
        move    #MOD_RING,r3
        move    #MIX_RING,r5
        nop
        move    x:(r3)+,a
        do      #BLOCK_FRAMES,ssm_done
        add     b,a
        and     y1,a
        move    a1,n0
        mac     x1,y1,b
        move    y:(r0+n0),y0
        mpy     x0,y0,a   y:(r5),y0
        add     y0,a      x:(r3)+,y0
        move    y0,a      a,y:(r5)+
ssm_done:
        move    b10,l:(r7)
        rts

; A self-modulated operator writing the modulation ring. The channel's two
; history products live in X (newer) and Y (older) internal memory; the
; frame's sum reads both, the newer ages into Y beside the add, and the new
; product lands in X beside the onward multiply. The gain pair alternates
; through a modulo-2 ring: history gain, then onward gain.
stage_mod_fb:
        move    #MOD_RING,r3
        move    x:render_hist,r2
        move    x:render_hist,r4
        move    x:render_gain,r5
        move    #1,m5
        nop
        move    y:(r5)+,y0
        do      #BLOCK_FRAMES,smf_done
        move    x:(r2),x0 y:(r4),a
        add     x0,a      x0,y:(r4)
        add     b,a
        and     y1,a
        move    a1,n0
        mac     x1,y1,b
        move    y:(r0+n0),x0
        mpy     x0,y0,a   y:(r5)+,y0
        mpy     x0,y0,a   a,x:(r2) y:(r5)+,y0
        move    a,x:(r3)+
smf_done:
        move    #>-1,m5
        move    b10,l:(r7)
        rts

; The same operator accumulating into the mix ring (additive connection).
stage_mod_fb_mix:
        move    r7,x:save_r7
        move    #MIX_RING,r7
        move    x:render_hist,r2
        move    x:render_hist,r4
        move    x:render_gain,r5
        move    #1,m5
        nop
        move    y:(r5)+,y0
        do      #BLOCK_FRAMES,smfm_done
        move    x:(r2),x0 y:(r4),a
        add     x0,a      x0,y:(r4)
        add     b,a
        and     y1,a
        move    a1,n0
        mac     x1,y1,b
        move    y:(r0+n0),x0
        mpy     x0,y0,a   y:(r5)+,y0
        mpy     x0,y0,a   a,x:(r2) y:(r5)+,y0
        move    y:(r7),x0
        add     x0,a
        move    a,y:(r7)+
smfm_done:
        move    #>-1,m5
        move    x:save_r7,r7
        nop
        move    b10,l:(r7)
        rts

; One operator: trigger, key, envelope, gains, increment. The record is
; walked forward with postincrements; the one indexed read is the rate of
; the current state, and the one backward step is the envelope's store.
;
; Enters with r1 at the record's TRIG and leaves it at the next record's;
; r3 walks the render parameters; r5 addresses the channel's history words.
; block_boundary holds the pass's constants in registers: r0 = DECAY_TABLE,
; r7 = GAIN_TABLE, n3 = RT_STRIDE, n5 = 3 (the release state),
; y0 = $010000 (>> 7, and one sustain step), y1 = $000800 (>> 12).
; Returns x1 = GAIN and x0 = GAINMOD; the feedback gain is the channel's to
; derive, since only a modulator has one. The record's GAIN, GAINMOD,
; GAINFB and INC words are not written: the render reads its parameters.
;
; An address register loaded by a move is not usable as a pointer by the
; next instruction, so every load of n0, n1 and n7 below has an instruction
; between it and its use.
op_boundary:
        move    x:(r1)+,a               ; TRIG
        move    x:(r1)+,b               ; TRIGSEEN; -> FLAGS
        cmp     a,b
        jne     trigger_attack          ; rare: external, returns to ob_keyed or ob_release
        move    x:(r1)+,n4              ; FLAGS; -> STATE
        jset    #0,n4,ob_keyed
        ; Idle test: key up, no key-on pending, envelope silent. Such an
        ; operator's gains are zero and nothing else it would compute is
        ; observable, so it costs a dozen instructions.
        move    (r1)+                   ; -> ENV
        move    x:(r1)-,b               ; ENV; -> STATE
        move    x:k_env_silent,x0
        cmp     x0,b
        jeq     <ob_idle
ob_release:
        move    n5,x:(r1)               ; key up: released, whatever it was
ob_keyed:
        move    x:(r1)+,n1              ; STATE; -> ENV
        move    x:(r1)+,b               ; ENV; -> SL
        move    x:(r1)+,x1              ; SL; -> RATE_A
        move    x:(r1+n1),n0            ; the rate of this state
        jset    #1,n1,ob_slide          ; sustain or release
        jset    #0,n1,ob_decay
        ; attack
        tst     b                       ; a zero envelope can leave attack at any rate
        jeq     <ob_attack_done
        move    n0,a
        tst     a
        jeq     <ob_env_store            ; rate zero holds exactly
        move    x:k_sixty,x0
        cmp     x0,a
        jge     <ob_env_store            ; maximum rate selected mid-attack also holds
        move    #>ATTACK_TABLE,r0
        move    b1,x1                   ; env
        move    x:(r0+n0),x0            ; retention factor
        mpy     x1,x0,b
        move    #>DECAY_TABLE,r0
        move    x:k_attack_done,x0
        cmp     x0,b
        jge     <ob_env_store            ; still attacking
ob_attack_done:
        clr     b
        move    #>1,x0
ob_state_store:
        move    #>OPR_STATE-OPR_RATE_A,n1
        nop
        move    x0,x:(r1+n1)            ; state = decay, or sustain
        jmp     <ob_snap                 ; (an envelope of zero passes it unchanged)
; The chip leaves decay when the envelope's top five bits equal the sustain
; level: below it the step may reach it, inside that one sustain step the
; envelope holds where it is, and past it (a level lowered under a running
; decay) the decay runs on to silence.
ob_decay:
        move    x:(r0+n0),x0            ; the step
        cmp     x1,b
        jge     <ob_decay_past
        add     x0,b
        cmp     x1,b
        jlt     <ob_snap
        move    x1,b                    ; reached: hold at SL, sustain
ob_decay_sustain:
        move    #>2,x0
        jmp     <ob_state_store
ob_decay_past:
        move    y0,a                    ; one sustain step, 16 << 12
        add     x1,a
        cmp     a,b
        jlt     <ob_decay_sustain
        add     x0,b
        jmp     <ob_snap
ob_slide:
        move    x:(r0+n0),x0            ; the step
        add     x0,b
ob_snap:
        move    x:k_env_snap,x0
        cmp     x0,b
        jlt     <ob_env_store
        move    x:k_env_silent,b
ob_env_store:                           ; b = env; r1 -> RATE_A
        move    (r1)-                   ; -> SL
        move    b1,x:-(r1)              ; ENV; -> ENV
        ; envOut = (env >> 12) + TLKSL (+ tremolo), clamped to 0x1ff
        move    #<OPR_TLKSL-OPR_ENV,n1
        move    b1,x1
        mpy     y1,x1,a   (r1)+n1       ; -> TLKSL
        move    x:(r1)+,x0              ; TLKSL; -> INCBASE
        add     x0,a
        jclr    #1,n4,ob_no_tremolo
        move    x:tremolo_value,x0
        add     x0,a
ob_no_tremolo:
        move    x:k_env_out_max,x0
        cmp     x0,a
        jle     <ob_gain_lookup
        move    x0,a
ob_gain_lookup:
        move    a1,n7
        move    x:(r1)+,a               ; INCBASE; -> the unused word
        move    x:(r7+n7),x1            ; GAIN
        mpy     x1,y0,b   (r1)+         ; GAINMOD = GAIN >> 7; -> WFBASE
        jclr    #2,n4,ob_no_vibrato
        move    x:vibrato_offset,n1
        nop
        move    x:(r1+n1),x0            ; the delta of this LFO position
        add     x0,a                    ; a1 wraps, as the host's sum does
ob_no_vibrato:
        move    #<OP_STRIDE+OPR_TRIG-OPR_WFBASE,n1
        move    a1,x:(r3)+              ; render parameters: INC
        move    x1,x:(r3)+              ; GAIN
        move    b1,x0
        move    x0,x:(r3)+              ; GAINMOD
        move    x:(r1)+n1,a             ; WFBASE; -> the next record's TRIG
        move    a1,x:(r3)+
        rts
ob_idle:                                ; r1 -> STATE
        move    #<OP_STRIDE+OPR_TRIG-OPR_STATE,n1
        clr     a         (r3)+n3       ; its render parameters stay stale
        move    a1,x1
        move    a1,x0
        move    (r1)+n1
        rts

; Stream emit: the mix ring plus this block's expanded PCM, doubled and
; limited, as interleaved stereo into the SSI ring, with a running checksum
; of the limited words for the exactness gate.
emit_block_stream:
        move    x:out_pointer,r2
        move    x:pcm_read,r1
        move    #MIX_RING,r5
        move    x:checksum,b
        move    x:master_gain,y0
        move    x:pcm_present,a
        tst     a
        jeq     ebs_silent
        do      #BLOCK_FRAMES,ebs_done
        move    x:(r1)+,x0 y:(r5)+,y1
        mpyr    y0,y1,a                 ; the mix scaled by the master gain: rounded,
                                        ; so that $7fffff passes it unchanged
        add     x0,a
        asl     a
        move    a,x:(r2)+
        move    a,x:(r2)+
        move    a,x1
        add     x1,b
ebs_done:
        move    r1,x:pcm_read
        jmp     ebs_store
ebs_silent:
        do      #BLOCK_FRAMES,ebs_silent_done
        move    y:(r5)+,y1
        mpyr    y0,y1,a
        asl     a
        move    a,x:(r2)+
        move    a,x:(r2)+
        move    a,x1
        add     x1,b
ebs_silent_done:
ebs_store:
        move    r2,x:out_pointer
        move    b1,x:checksum
        rts

hot_code_end:
        nop                             ; never executed: gives the label an address

; =========================================================== external code
        org     p:$2000

start:
        movep   #1,x:m_pbc              ; Falcon host port
        movep   #0,x:m_bcr              ; reset leaves fifteen wait states
        move    #>-1,m0
        move    #>-1,m1
        move    #>-1,m2
        move    #>-1,m3
        move    #>-1,m4
        move    #>-1,m5
        move    #>-1,m6
        move    #>-1,m7
        ; internal RAM is undefined after boot: clear everything the kernel reads
        move    #0,r0
        move    #0,r4
        clr     a
        do      #256,start_cleared
        move    a1,x:(r0)+
        move    a1,y:(r4)+
start_cleared:
        move    #>4,a
        move    a1,x:tremolo_shift
        move    #>9,a
        move    a1,x:channel_count
        move    #>$7fffff,a
        move    a1,x:master_gain
        move    #>emit_block,a
        move    a1,x:emit_routine
        move    #>60,a
        move    a1,x:k_sixty
        move    #>ATTACK_DONE,a
        move    a1,x:k_attack_done
        move    #>ENV_SILENT,a
        move    a1,x:k_env_silent
        move    #>ENV_SNAP,a
        move    a1,x:k_env_snap
        move    #>$1ff,a
        move    a1,x:k_env_out_max
        jsr     rewind

command_loop:
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,a
        move    a1,x:last_command
        move    a1,x1
        move    #>$000080,y0
        mpy     x1,y0,a                 ; opcode = command >> 16
        move    a1,b

        move    #>CMD_PING,x0
        cmp     x0,b
        jne     try_write_x
        move    #>REPLY_PING,a
        jsr     host_send
        jmp     command_loop

; Every host word is acknowledged, so TOS's Dsp_BlkUnpacked never sends
; two words without a handshake.
try_write_x:
        move    #>CMD_WRITE_X,x0
        cmp     x0,b
        jne     try_write_y
        jsr     host_ack
        jsr     host_receive
        move    a1,r0
        jsr     host_ack
        jsr     host_receive
        move    a1,b
        jsr     host_ack
        tst     b
        jeq     command_loop
        clr     a
write_x_loop:
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,x:(r0)+
        jclr    #1,x:m_hsr,*
        movep   a1,x:m_htx
        move    #>1,x0
        sub     x0,b
        jne     write_x_loop
        jmp     command_loop

try_write_y:
        move    #>CMD_WRITE_Y,x0
        cmp     x0,b
        jne     try_events
        jsr     host_ack
        jsr     host_receive
        move    a1,r4
        jsr     host_ack
        jsr     host_receive
        move    a1,b
        jsr     host_ack
        tst     b
        jeq     command_loop
        clr     a
write_y_loop:
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,y:(r4)+
        jclr    #1,x:m_hsr,*
        movep   a1,x:m_htx
        move    #>1,x0
        sub     x0,b
        jne     write_y_loop
        jmp     command_loop

try_events:
        move    #>CMD_EVENTS,x0
        cmp     x0,b
        jne     try_render
        jsr     command_argument
        move    a1,x:event_count
        move    #>EVENT_BASE,a
        move    a1,x:event_read
        jmp     command_ack

try_render:
        move    #>CMD_RENDER,x0
        cmp     x0,b
        jne     try_read_x
        jsr     command_argument
        move    a1,x1
        tst     a
        jeq     command_ack
profile_start:
        do      x1,render_done
        jsr     render_block
        nop
render_done:
        jmp     command_ack

try_read_x:
        move    #>CMD_READ_X,x0
        cmp     x0,b
        jne     try_rewind
        jsr     command_argument
        move    a1,r0
        nop
        nop
        move    x:(r0),a
        jsr     host_send
        jmp     command_loop

try_rewind:
        move    #>CMD_REWIND,x0
        cmp     x0,b
        jne     try_stop
        jsr     rewind
        jmp     command_ack

try_stop:
        move    #>CMD_STOP,x0
        cmp     x0,b
        jne     try_stream_start
profile_end:
        jmp     command_ack

try_stream_start:
        move    #>CMD_STREAM_START,x0
        cmp     x0,b
        jne     command_unknown
        jmp     command_stream_start

command_unknown:
        move    #>REPLY_ERROR,a
        jsr     host_send
        jmp     command_loop

command_ack:
        clr     a
        jsr     host_send
        jmp     command_loop

rewind:
        move    #>FRAME_BASE,a
        move    a1,x:frame_pointer
        move    #>EVENT_BASE,a
        move    a1,x:event_read
        clr     a
        move    a1,x:event_count
        move    a1,x:block_index
        rts

command_argument:
        move    x:last_command,a
        move    #>$00ffff,x0
        and     x0,a
        move    a1,a
        rts

host_send:
        jsclr   #1,x:m_hsr,wait_tx
        movep   a1,x:m_htx
        rts

host_ack:
        jsclr   #1,x:m_hsr,wait_tx
        movep   #>0,x:m_htx
        rts

host_receive:
        jsclr   #0,x:m_hsr,wait_rx
        movep   x:m_hrx,a
        rts

; ------------------------------------------------- per-block code, external P
; Everything here runs once per block or once per channel per block; only
; the per-frame stages and the per-operator boundary pass earn internal P.

; ------------------------------------------------------- render driver

; Rare key-on work lives outside the internal render loops. Entered by a
; jump from op_boundary with a = TRIG and r1 at FLAGS; returns into it past
; the idle test, which a pending key-on fails.
trigger_attack:
        move    (r1)-                   ; -> TRIGSEEN
        move    a1,x:(r1)-              ; TRIGSEEN = TRIG; -> TRIG
        clr     a         (r1)-         ; -> PHASE
        move    a1,x:(r5)               ; history to zero
        move    a1,y:(r5)
        move    a10,l:(r1)              ; phase to zero
        move    #<OPR_STATE,n1
        move    x:k_sixty,x0
        move    a1,x:(r1+n1)            ; state = attack
        move    #<OPR_RATE_A,n1
        nop
        move    x:(r1+n1),b
        cmp     x0,b
        jlt     ta_done
        move    #<OPR_ENV,n1
        nop
        move    a1,x:(r1+n1)            ; maximum-rate key-on: no attenuation
ta_done:
        move    #<OPR_FLAGS,n1
        nop
        move    (r1)+n1                 ; -> FLAGS
        move    x:(r1)+,n4              ; FLAGS; -> STATE
        jset    #0,n4,ob_keyed
        jmp     ob_release

render_channels:
        move    #>CH_BASE,a
        move    a1,x:render_ch
        move    #>RT_BASE,a
        move    a1,x:render_rt
        move    #>OP_BASE,a
        move    a1,x:render_op
        move    #>HIST_BASE,a
        move    a1,x:render_hist
        move    #>GAIN_RING,a
        move    a1,x:render_gain
        move    #>$3ff,y1
        do      x:channel_count,rc_done
        move    x:render_ch,r0
        nop
        move    x:(r0),r0
        nop
        jsr     (r0)
        move    x:render_ch,a
        move    #>CH_STRIDE,x0
        add     x0,a
        move    a1,x:render_ch
        move    x:render_rt,a
        move    #>2*RT_STRIDE,x0
        add     x0,a
        move    a1,x:render_rt
        move    x:render_op,a
        move    #>2*OP_STRIDE,x0
        add     x0,a
        move    a1,x:render_op
        move    x:render_hist,a
        move    #>1,x0
        add     x0,a
        move    a1,x:render_hist
        move    x:render_gain,a
        move    #>2,x0
        add     x0,a
        move    a1,x:render_gain
        nop
rc_done:
        rts

; ------------------------------------------------------ operator loaders
;
; Each fills B, x1, r0 and a gain register from the channel's render
; parameters: INC, GAIN, GAINMOD, WFBASE per operator.

load_mod:                               ; y0 = mix gain
        move    x:render_op,r7
        move    x:render_rt,r1
        nop
        move    l:(r7),b10
        move    x:(r1)+,x1
        move    x:(r1)+,y0
        move    x:(r1)+,a
        move    x:(r1)+,r0
        rts

load_mod_ring:                          ; y0 = ring gain
        move    x:render_op,r7
        move    x:render_rt,r1
        nop
        move    l:(r7),b10
        move    x:(r1)+,x1
        move    x:(r1)+,a
        move    x:(r1)+,y0
        move    x:(r1)+,r0
        rts

load_carrier:                           ; y0 = mix gain
        move    x:render_op,a
        move    #>OP_STRIDE,x0
        add     x0,a
        move    a1,r7
        move    x:render_rt,a
        move    #>RT_STRIDE,x0
        add     x0,a
        move    a1,r1
        move    l:(r7),b10
        move    x:(r1)+,x1
        move    x:(r1)+,y0
        move    x:(r1)+,a
        move    x:(r1)+,r0
        rts

load_carrier_x0:                        ; x0 = mix gain, for the serial stage
        move    x:render_op,a
        move    #>OP_STRIDE,x0
        add     x0,a
        move    a1,r7
        move    x:render_rt,a
        move    #>RT_STRIDE,x0
        add     x0,a
        move    a1,r1
        move    l:(r7),b10
        move    x:(r1)+,x1
        move    x:(r1)+,x0
        move    x:(r1)+,a
        move    x:(r1)+,r0
        rts

; -------------------------------------------------------- channel modes

mode_skip:
        rts

mode_carrier_only:
        jsr     load_carrier
        jmp     stage_indep_mix

mode_fm_fb:
        jsr     load_mod_ring
        jsr     stage_mod_fb
        jsr     load_carrier_x0
        jmp     stage_serial_mix

mode_fm_plain:
        jsr     load_mod_ring
        jsr     stage_mod_plain
        jsr     load_carrier_x0
        jmp     stage_serial_mix

mode_add_fb:
        jsr     load_mod
        jsr     stage_mod_fb_mix
        jsr     load_carrier
        jmp     stage_indep_mix

mode_add_plain:
        jsr     load_mod
        jsr     stage_indep_mix
        jsr     load_carrier
        jmp     stage_indep_mix

mode_mod_only_fb:
        jsr     load_mod
        jmp     stage_mod_fb_mix

mode_mod_only_plain:
        jsr     load_mod
        jmp     stage_indep_mix

clear_mix:
        move    #MIX_RING,r5
        clr     a
        rep     #BLOCK_FRAMES
        move    a,y:(r5)+
        rts

; The mix ring doubled and limited into the bench output, two frames per
; pair of instructions: each shifts one accumulator while the other is
; stored and refilled.
emit_block:
        move    x:frame_pointer,r1
        move    #MIX_RING,r5
        move    x:master_gain,y0
        do      #BLOCK_FRAMES,eb_done
        move    y:(r5)+,y1
        mpyr    y0,y1,a
        asl     a
        move    a,x:(r1)+
eb_done:
        move    r1,x:frame_pointer
        rts

; ---------------------------------------------------- block boundary pass
;
; Advances the LFO, then every operator's envelope and gains, then decides
; each channel's render mode. The per-operator pass walks the record
; sequentially; r1 = record, r2 = channel record, r3 = render parameters,
; r4 = gain pair, r5 = history word.

block_boundary:
        ; tremolo: a 210-step triangle at 3.7 Hz, positions in 12 fraction bits
        move    x:tremolo_phase,a
        move    #>OPL_TREMOLO_STEP,x0
        add     x0,a
        move    #>210*4096,x0
        cmp     x0,a
        jlt     bb_trem_wrapped
        sub     x0,a
bb_trem_wrapped:
        move    a1,x:tremolo_phase
        move    a1,x1
        move    #>$000800,y0            ; >> 12
        mpy     x1,y0,a
        move    a1,a                    ; the integer position alone
        move    #>105,x0
        cmp     x0,a
        jlt     bb_trem_rising
        move    #>210,b
        sub     a,b
        move    b1,a
bb_trem_rising:
        move    a1,x1
        move    #>$200000,y0            ; >> 2
        move    x:tremolo_shift,b
        move    #>2,x0
        cmp     x0,b
        jeq     bb_trem_scaled
        move    #>$080000,y0            ; >> 4
bb_trem_scaled:
        mpy     x1,y0,a
        move    a1,x:tremolo_value

        ; vibrato: eight positions at 6.1 Hz, each selecting one of the
        ; record's increment deltas
        move    x:vibrato_phase,a
        move    #>OPL_VIBRATO_STEP,x0
        add     x0,a
        move    #>8*4096,x0
        cmp     x0,a
        jlt     bb_vib_wrapped
        sub     x0,a
bb_vib_wrapped:
        move    a1,x:vibrato_phase
        move    a1,x1
        move    #>$000800,y0
        mpy     x1,y0,a                 ; position 0..7
        move    a1,a
        move    #>VIB_TABLE,x0
        add     x0,a
        move    a1,r0
        move    #>OPR_WFBASE,x0
        move    x:(r0),a
        sub     x0,a                    ; op_boundary indexes from WFBASE
        move    a1,x:vibrato_offset

        move    #>OP_BASE+OPR_TRIG,r1
        move    #>CH_BASE,r2
        move    #>RT_BASE,r3
        move    #>GAIN_RING,r4
        move    #>HIST_BASE,r5
        ; what op_boundary keeps in registers across the pass
        move    #>DECAY_TABLE,r0
        move    #>GAIN_TABLE,r7
        move    #<RT_STRIDE,n3
        move    #<3,n5
        move    #>$010000,y0
        move    #>$000800,y1
        do      x:channel_count,bb_channels_done
        jsr     channel_boundary
        nop
bb_channels_done:
        rts

channel_boundary:
        move    #<CHR_FBMUL,n2
        jsr     op_boundary             ; the modulator; r1 advances to the carrier
        move    x1,x:ob_gain_mod
        move    x0,x:ob_gainmod_mod
        move    x:(r2+n2),x0
        mpy     x1,x0,a                 ; gain * 2^(fb - 16), zero without feedback
        move    a1,x:ob_gainfb_mod
        jsr     op_boundary             ; the carrier
        move    x1,x:ob_gain_car

        move    x:(r2+n2),b             ; nonzero: feedback
        move    (r2)+
        move    x:(r2)-,a               ; CHR_CONN
        tst     a
        jne     cb_additive
        move    x:ob_gain_car,a
        tst     a
        jeq     cb_skip
        move    x:ob_gain_mod,a
        tst     a
        jeq     cb_carrier_only
        tst     b
        jeq     cb_fm_plain
        move    #>mode_fm_fb,a
        move    x:ob_gainmod_mod,x0     ; pair: history gain, ring gain
        jmp     cb_store_pair
cb_fm_plain:
        move    #>mode_fm_plain,a
        jmp     cb_store
cb_additive:
        move    x:ob_gain_mod,a
        tst     a
        jne     cb_add_mod_on
        move    x:ob_gain_car,a
        tst     a
        jeq     cb_skip
cb_carrier_only:
        move    #>mode_carrier_only,a
        jmp     cb_store
cb_add_mod_on:
        move    x:ob_gain_car,a
        tst     a
        jeq     cb_mod_only
        tst     b
        jeq     cb_add_plain
        move    #>mode_add_fb,a
        move    x:ob_gain_mod,x0        ; pair: history gain, mix gain
        jmp     cb_store_pair
cb_add_plain:
        move    #>mode_add_plain,a
        jmp     cb_store
cb_mod_only:
        tst     b
        jeq     cb_mod_only_plain
        move    #>mode_mod_only_fb,a
        move    x:ob_gain_mod,x0
        jmp     cb_store_pair
cb_mod_only_plain:
        move    #>mode_mod_only_plain,a
        jmp     cb_store
cb_skip:
        move    #>mode_skip,a
        clr     b
        move    b1,x:(r5)
        move    b1,y:(r5)
        jmp     cb_store
cb_store_pair:
        move    x:ob_gainfb_mod,x1
        move    x1,y:(r4)
        move    #>1,n4
        nop
        move    x0,y:(r4+n4)
cb_store:
        move    a1,x:(r2)               ; CHR_MODE
        move    #>CH_STRIDE,n2
        move    #>2,n4
        move    (r2)+n2
        move    (r4)+n4
        move    (r5)+
        rts

; ------------------------------------------------------------ one block

render_block:
        jsr     apply_events
        jset    #0,x:fm_paused,rb_paused
        jsr     block_boundary
        jsr     clear_mix
        jsr     render_channels
rb_emit:
        move    x:emit_routine,r0
        nop
        jsr     (r0)
        move    x:block_index,a
        move    #>1,x0
        add     x0,a
        move    a1,x:block_index
        rts
rb_paused:
        jsr     clear_mix              ; no FM or synth-state advance, but PCM still plays
        jmp     rb_emit

; Apply every pending event due at this block. The table is sorted by
; block; each event is (block << 16 | X address) followed by the value.
apply_events:
        move    x:event_count,a
        tst     a
        jeq     ae_done
        move    x:event_read,r0
        nop
ae_loop:
        move    x:(r0),a
        move    a1,x1
        move    #>$000080,y0
        mpy     x1,y0,a                 ; event block = word >> 16
        move    a1,a                    ; drop the shifted-out address bits
        move    x:block_index,x0
        cmp     x0,a
        jne     ae_store_pointer
        move    (r0)+
        move    x1,a
        move    #>$00ffff,x0
        and     x0,a
        move    a1,r1
        move    x:(r0)+,x0              ; value
        move    x:event_count,a
        move    x0,x:(r1)
        move    #>1,x0
        sub     x0,a
        move    a1,x:event_count
        jne     ae_loop
ae_store_pointer:
        move    r0,x:event_read
ae_done:
        rts



; ------------------------------------------------------------ stream mode
;
; The DSP owns the codec: a 1,920-word SSI ring holds two 480-frame periods
; of interleaved stereo, r6/m6 transmit it under interrupt, and the kernel
; renders each period into the half the transmitter has just left. A period
; arrives from the host as events plus 160 mono PCM samples (a third of the
; codec rate, expanded here by linear interpolation) and is rendered as
; soon as its half is free. A period that is not ready in time repeats the
; old audio; track_halves watches the transmitter to count each one.

command_stream_start:
        movep   #0,x:m_crb
        movep   #$4100,x:m_cra          ; 16-bit words, two per frame
        movep   #$1f8,x:m_pcc           ; port C pins to the SSI
        movep   #$3000,x:m_ipr          ; SSI interrupt priority 2
        andi    #$fc,mr                 ; unmask it
        move    #SSI_RING,r0
        clr     a
        do      #SSI_RING_WORDS,css_cleared
        move    a1,x:(r0)+
css_cleared:
        move    a1,x:periods_rendered
        move    a1,x:late_periods
        move    a1,x:stream_primed
        move    a1,x:fresh_a
        move    a1,x:fresh_b
        move    a1,x:stream_live
        move    a1,x:tx_half_seen       ; r6 starts in half A
        move    a1,x:checksum
        move    a1,x:pcm_previous
        move    a1,x:pcm_present
        move    #>1,a
        move    a1,x:stream_next_half   ; half A plays silence first
        move    #>emit_block_stream,a
        move    a1,x:emit_routine
        move    #SSI_RING,r6
        move    #>SSI_RING_WORDS-1,m6
        nop
        move    x:(r6)+,a
        movep   a1,x:m_tx
        movep   #$5a00,x:m_crb          ; network, transmit, interrupt
        move    #>1,a
        move    a1,x:tracking
        jsr     host_ack

stream_loop:
        jsclr   #0,x:m_hsr,wait_rx
        movep   x:m_hrx,a
        move    a1,x:last_command
        move    a1,x1
        move    #>$000080,y0
        mpy     x1,y0,a
        move    a1,b
        move    #>CMD_REFILL,x0
        cmp     x0,b
        jeq     command_refill
        move    #>CMD_STREAM_STOP,x0
        cmp     x0,b
        jeq     command_stream_stop
        move    #>CMD_STATUS,x0
        cmp     x0,b
        jeq     command_status
        move    #>CMD_CHECKSUM,x0
        cmp     x0,b
        jeq     command_checksum
        move    #>CMD_PING,x0
        cmp     x0,b
        jne     stream_unknown
        move    #>REPLY_PING,a
        jsr     host_send
        jmp     stream_loop
stream_unknown:
        move    #>REPLY_ERROR,a
        jsr     host_send
        jmp     stream_loop

; One period: READY parks the receiver, then the event count, the events,
; the PCM flag and the samples arrive; the acknowledgement, sent before
; rendering so the host can prepare the next period meanwhile, carries the
; period and late counters as the status query does.
command_refill:
        move    #>REPLY_READY,a
        jsr     host_send
        jsr     host_receive
        move    a1,x:event_count
        move    #>STREAM_EVENT_BASE,b
        move    b1,x:event_read
        move    #>STREAM_EVENT_BASE,r0
        tst     a
        jeq     refill_events_done
        do      a1,refill_events_done
        jsclr   #0,x:m_hsr,wait_rx
        movep   x:m_hrx,x:(r0)+
        jsclr   #0,x:m_hsr,wait_rx
        movep   x:m_hrx,x:(r0)+
refill_events_done:
        jsr     host_receive
        move    a1,x:pcm_present
        tst     a
        jne     refill_pcm_present
        move    a1,x:pcm_previous       ; omitted samples are zeros, including the last one
        jmp     refill_pcm_done
refill_pcm_present:
        jsr     receive_pcm
refill_pcm_done:
        jsr     send_status             ; the acknowledgement carries the counters
        jsr     render_period
        jmp     stream_loop

; 192 samples, each 16 bits in bits 7-22 of its word, expanded to 768
; frames by linear interpolation from the previous sample.
receive_pcm:
        move    #PCM_STAGE,r1
        move    x:pcm_previous,b
        move    #>$200000,y0            ; one quarter
        do      #PCM_PER_PERIOD,rp_done
        jsclr   #0,x:m_hsr,wait_rx
        movep   x:m_hrx,a
        move    a1,x0
        sub     b,a
        move    a1,x1                   ; difference
        mpy     x1,y0,a
        move    a1,x1                   ; a quarter of it
        move    b1,a
        add     x1,a
        move    a1,x:(r1)+
        add     x1,a
        move    a1,x:(r1)+
        add     x1,a
        move    a1,x:(r1)+
        move    x0,x:(r1)+
        move    x0,b
rp_done:
        move    b1,x:pcm_previous
        rts

; Wait for the transmitter to leave the half about to be rendered, render
; the period into it, and mark the half fresh unless the transmitter caught
; up; track_halves does the counting.
;
; The first period arrives whenever the host gets round to it, with the
; transmitter anywhere in the silent ring. It first waits for the
; transmitter to be inside the half it will render, so that the wait below
; hands it a whole half like every later period; otherwise the stream would
; always open with a late period.
render_period:
        move    x:stream_primed,a
        tst     a
        jne     rp_wait
        move    #>1,a
        move    a1,x:stream_primed
rp_first:
        jsr     track_halves
        move    x:stream_next_half,b
        move    r6,a
        move    #>SSI_RING+SSI_HALF_WORDS,x0
        tst     b
        jeq     rp_first_a
        cmp     x0,a                    ; rendering B: wait until r6 is in B
        jlt     rp_first
        jmp     rp_wait
rp_first_a:
        cmp     x0,a                    ; rendering A: wait until r6 is in A
        jge     rp_first
rp_wait:
        jsr     track_halves
        move    x:stream_next_half,b
        move    r6,a
        move    #>SSI_RING+SSI_HALF_WORDS,x0
        tst     b
        jeq     rp_wait_for_b
        cmp     x0,a                    ; rendering B: wait while r6 is in B
        jge     rp_wait
        move    #>SSI_RING+SSI_HALF_WORDS,a
        jmp     rp_go
rp_wait_for_b:
        cmp     x0,a                    ; rendering A: wait while r6 is in A
        jlt     rp_wait
        move    #>SSI_RING,a
rp_go:
        move    a1,x:out_pointer
        move    #>PCM_STAGE,a
        move    a1,x:pcm_read
        clr     a
        move    a1,x:block_index
        do      #PERIOD_BLOCKS,rp_rendered
        jsr     render_block
        jsr     track_halves            ; so a crossing mid-render is seen within a block
        nop
rp_rendered:
        ; The period is judged against the tracker's own view of the
        ; transmitter, not a second look at r6, so the two cannot disagree:
        ; if the transmitter entered this half before the render finished,
        ; track_halves has seen it and counted the period late on entry;
        ; otherwise the half is fresh, and its entry is on time whenever it
        ; comes.
        jsr     track_halves
        move    x:stream_next_half,b
        move    x:tx_half_seen,a
        cmp     b,a
        jeq     rp_counted              ; caught mid-render: already late
        move    #>1,a
        tst     b
        jeq     rp_fresh_a
        move    a1,x:fresh_b
        jmp     rp_counted
rp_fresh_a:
        move    a1,x:fresh_a
rp_counted:
        move    x:stream_next_half,a
        move    #>1,x0
        eor     x0,a
        move    a1,x:stream_next_half
        move    x:periods_rendered,a
        add     x0,a
        move    a1,x:periods_rendered
        rts

command_status:
        jsr     send_status
        jmp     stream_loop

; late periods << 12 | periods rendered, as a reply word
send_status:
        move    x:periods_rendered,a
        move    #>$000fff,x0
        and     x0,a
        move    x:late_periods,b
        rep     #12
        asl     b
        add     b,a
        jmp     host_send

command_checksum:
        move    x:checksum,a
        jsr     host_send
        jmp     stream_loop

command_stream_stop:
stream_stopped:
        clr     a
        move    a1,x:tracking
        movep   #0,x:m_crb
        move    #>-1,m6
        clr     a
        movep   a1,x:m_tx
        move    #>emit_block,a
        move    a1,x:emit_routine
        jsr     host_ack
        jmp     command_loop

; ----------------------------------------------------- transmitter tracking
;
; The transmitter walks the ring under interrupt and never stops: when the
; host is late with a period, it plays the old audio in the ring again. Only
; watching it can count that, since nothing is rendered while the host
; stalls. track_halves notes each crossing from one half into the other and
; holds it against that half's flag, which a render sets when it finishes
; before the transmitter gets there: a fresh half is used up, and any other -
; a replay while the host stalls, or a period the transmitter caught
; mid-render - counts one late period. Counting starts once the first
; rendered period has played, so the silence the stream opens on is not
; counted.
;
; It is called from every wait while streaming and between the blocks of a
; render, so it never goes a whole half without looking. It keeps a, x0 and
; every other register, and changes only the condition codes.
track_halves:
        jclr    #0,x:tracking,th_off
        move    a2,x:th_a2
        move    a1,x:th_a1
        move    a0,x:th_a0
        move    x0,x:th_x0
        move    r6,a
        move    #>SSI_RING+SSI_HALF_WORDS,x0
        cmp     x0,a
        move    #>0,a                   ; a move keeps the comparison's flags
        jlt     th_half
        move    #>1,a
th_half:
        move    x:tx_half_seen,x0
        cmp     x0,a
        jeq     th_done                 ; still in the same half
        move    a1,x:tx_half_seen
        tst     a
        jne     th_entered_b
        move    x:fresh_a,a
        tst     a
        jeq     th_stale
        clr     a
        move    a1,x:fresh_a
        jmp     th_fresh
th_entered_b:
        move    x:fresh_b,a
        tst     a
        jeq     th_stale
        clr     a
        move    a1,x:fresh_b
th_fresh:
        move    #>1,a
        move    a1,x:stream_live
        jmp     th_done
th_stale:
        move    x:stream_live,a
        tst     a
        jeq     th_done                 ; still the opening silence
        move    x:late_periods,a
        move    #>1,x0
        add     x0,a
        move    a1,x:late_periods
th_done:
        move    x:th_x0,x0
        move    x:th_a0,a0
        move    x:th_a1,a1
        move    x:th_a2,a2
th_off:
        rts

; Waits on the host that keep tracking the transmitter. Reached through
; jsclr only while the port is not ready, so a ready port costs nothing.
wait_rx:
        jsr     track_halves
        jclr    #0,x:m_hsr,wait_rx
        rts

wait_tx:
        jsr     track_halves
        jclr    #1,x:m_hsr,wait_tx
        rts

ssi_tx_exception:
        movep   x:m_sr,x:ssi_status
        movep   x:(r6)+,x:m_tx
        rti

external_code_end:
        nop                             ; never executed: gives the label an address

        end
