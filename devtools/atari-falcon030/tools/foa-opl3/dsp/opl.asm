; OPL two-operator synthesis kernel for the Falcon's DSP56001.
;
; A benchmark and exactness fixture, not a driver: it owns no SSI, no
; interrupt and no transport. It renders the same arithmetic as the host
; opl-kernel.h, which is checked sample for sample against Nuked-OPL3, so its
; frames can be compared word for word against that reference.
;
; Memory map
;   P internal $0000-$01ff  all code; instruction fetch never leaves the chip
;   X internal $0000-$003f  scalars, inside the six-bit short address range
;   X internal $0040-$007f  shift constants 2^(22-k) for k = 0..63
;   X internal $0080-$00c7  per-operator contribution scratch, 36 words
;   Y internal $0000-$00ff  exponential ROM, pre-doubled
;   X external $0400-$063f  operator state, 16 words each, 36 operators
;   X external $1000-$2fff  waveform magnitudes, 1024 words per waveform
;   Y external $1000-$2fff  waveform sign masks, at the same index
;   X external $3000-$3fff  rendered frames, right then left per frame
;
; The Falcon's DSP SRAM is zero wait states once the bus control register is
; cleared, which reset does not do on the Dsp_ExecBoot path.
;
; Everything is scaled to fit a 24-bit word. The host phase is nineteen bits
; and only its top ten reach the waveform, so the DSP holds it shifted left
; five and takes the index from bits 14-23. A right shift by k is one multiply
; by 2^(22-k) against a pre-doubled operand, which keeps every table constant
; inside the signed 24-bit range and costs one instruction instead of a
; repeated single-bit shift.

        include 'ioequ.inc'

OPS_OUT         equ     0
OPS_PREV        equ     1
OPS_FBGAIN      equ     2       ; 2^(22-(9-fb)) against doubled input, or zero
OPS_MODMASK     equ     3       ; carrier: admits the modulator, or blocks it
OPS_INC         equ     4
OPS_PHASE       equ     5
OPS_WFBASE      equ     6
OPS_ENV8        equ     7       ; envelope output times eight
OPS_CONTRIB     equ     8       ; $ffffff when this operator reaches the mix
OPS_STRIDE      equ     9       ; the body walks the whole record, so no fixup

OP_STATE        equ     $0400
WAVE_BASE       equ     $1000
FRAME_BASE      equ     $3000

SHIFT_TABLE     equ     $0040
CONTRIB_SCRATCH equ     $0080

CMD_PING        equ     $01
CMD_WRITE_X     equ     $02     ; address word, count word, then the data
CMD_WRITE_Y     equ     $03
CMD_CHANNELS    equ     $04
CMD_DELAYED     equ     $05     ; arg = number of left-delayed carriers
CMD_RENDER      equ     $06     ; arg = frame count
CMD_READ_X      equ     $07     ; arg = address
CMD_REWIND      equ     $08
CMD_RDELAYED    equ     $09     ; arg = right-delayed carriers, slot 33 and above
REPLY_PING      equ     $4f504c ; "OPL"
REPLY_ERROR     equ     $ffffff

        org     x:$0000
last_command:   ds      1
channel_count:  ds      1
delayed_count:  ds      1
frame_pointer:  ds      1
right_pending:  ds      1
ldelay_prev:    ds      1
frame_sum:      ds      1
carrier_mod:    ds      1
left_mix:       ds      1
const_shift9:   ds      1       ; 2^9,  phase word to table index
const_shift15:  ds      1       ; 2^15, level to exponent
const_shift7:   ds      1       ; 2^7,  command word to opcode
const_mask3ff:  ds      1
const_maskff:   ds      1
const_maskffff: ds      1
carrier_first:  ds      1       ; scratch address of the first left-delayed carrier
rdelayed_count: ds      1       ; carriers the right mix also takes a frame late
rcarrier_first: ds      1
rdelay_prev:    ds      1

        org     p:$0000
        jmp     start

        org     p:$0040

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
        move    #>$000200,a
        move    a1,x:const_shift9
        move    #>$008000,a
        move    a1,x:const_shift15
        move    #>$000080,a
        move    a1,x:const_shift7
        move    #>$0003ff,a
        move    a1,x:const_mask3ff
        move    #>$0000ff,a
        move    a1,x:const_maskff
        move    #>$00ffff,a
        move    a1,x:const_maskffff
        move    #>9,a
        move    a1,x:channel_count
        clr     a
        move    a1,x:delayed_count
        move    a1,x:rdelayed_count
        move    a1,x:right_pending
        move    a1,x:ldelay_prev
        move    a1,x:rdelay_prev
        move    #>FRAME_BASE,a
        move    a1,x:frame_pointer

command_loop:
        jclr    #0,x:m_hsr,*            ; host receive data full
        movep   x:m_hrx,a
        move    a1,x:last_command
        move    a1,x1
        move    x:const_shift7,y0
        mpy     x1,y0,a                 ; opcode = command >> 16
        move    a1,b

        move    #>CMD_PING,x0
        cmp     x0,b
        jne     try_write_x
        move    #>REPLY_PING,a
        jsr     host_send
        jmp     command_loop

; Every host word is acknowledged, so the host never has to send two words
; without a handshake. TOS's Dsp_BlkUnpacked only handshakes its first word,
; which makes an unacknowledged bulk send unsafe.
try_write_x:
        move    #>CMD_WRITE_X,x0
        cmp     x0,b
        jne     try_write_y
        jsr     host_ack                ; the command word itself
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
        jne     try_channels
        jsr     host_ack                ; the command word itself
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

try_channels:
        move    #>CMD_CHANNELS,x0
        cmp     x0,b
        jne     try_delayed
        jsr     command_argument
        move    a1,x:channel_count
        jmp     command_ack

try_delayed:
        move    #>CMD_DELAYED,x0
        cmp     x0,b
        jne     try_rdelayed
        jsr     command_argument
        move    a1,x:delayed_count
        move    a1,x0
        move    x:channel_count,a
        sub     x0,a                    ; index of the first delayed channel
        asl     a                       ; two scratch words per channel
        move    #>CONTRIB_SCRATCH+1,x0  ; carriers occupy the odd scratch words
        add     x0,a
        move    a1,x:carrier_first
        jmp     command_ack

try_rdelayed:
        move    #>CMD_RDELAYED,x0
        cmp     x0,b
        jne     try_render
        jsr     command_argument
        move    a1,x:rdelayed_count
        move    a1,x0
        move    x:channel_count,a
        sub     x0,a
        asl     a
        move    #>CONTRIB_SCRATCH+1,x0
        add     x0,a
        move    a1,x:rcarrier_first
        jmp     command_ack

try_render:
        move    #>CMD_RENDER,x0
        cmp     x0,b
        jne     try_read_x
        jsr     command_argument
        jsr     render_frames
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
        jne     command_unknown
        move    #>FRAME_BASE,a
        move    a1,x:frame_pointer
        clr     a
        move    a1,x:right_pending
        move    a1,x:ldelay_prev
        move    a1,x:rdelay_prev
        jmp     command_ack

command_unknown:
        move    #>REPLY_ERROR,a
        jsr     host_send
        jmp     command_loop

command_ack:
        clr     a
        jsr     host_send
        jmp     command_loop

command_argument:
        move    x:last_command,a
        move    x:const_maskffff,x0
        and     x0,a
        move    a1,a
        rts

host_send:
        jclr    #1,x:m_hsr,*            ; host transmit data empty
        movep   a1,x:m_htx
        rts

host_ack:
        jclr    #1,x:m_hsr,*
        movep   #>0,x:m_htx
        rts

host_receive:
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,a
        rts

; ---------------------------------------------------------- operator macros
;
; The bodies are inlined rather than called. A jsr, a jmp and an rts cost ten
; instruction cycles per operator between them, which is real money against a
; budget of a few hundred cycles for the whole frame.
;
; Common tail. y1 carries this operator's modulation and r7 its state base;
; r1 walks the record and lands on the next operator, r3 walks the
; contribution scratch, and b carries the running sum over every operator.

opbody  macro
        move    x:(r1)+,y0              ; INC
        move    x:(r1),a                ; PHASE
        move    a1,x1                   ; the phase before this step
        add     y0,a
        move    a1,x:(r1)+              ; PHASE advanced
        move    x:const_shift9,y0
        mpy     x1,y0,a                 ; table index from bits 14-23
        move    y1,x0
        add     x0,a                    ; plus modulation
        move    x:const_mask3ff,x0
        and     x0,a
        move    a1,a
        move    x:(r1)+,x0              ; WFBASE
        add     x0,a
        move    a1,r0
        move    a1,r4
        move    x:(r1)+,y1              ; ENV8, loaded while r0 and r4 settle
        move    x:(r0),x0               ; waveform magnitude, external X
        move    y1,a
        add     x0,a                    ; level
        move    a1,x1
        move    x:const_shift15,y0
        mpy     x1,y0,a                 ; exponent
        move    a1,r2
        move    x1,a                    ; the level again
        move    x:const_maskff,x0
        and     x0,a
        move    a1,r6
        move    y:(r4),y1               ; waveform sign mask, external Y
        move    x:(r2+n2),x1            ; shift constant
        move    y:(r6),y0               ; exponential value, pre-doubled
        mpy     x1,y0,a
        eor     y1,a                    ; apply the sign
        move    a1,x:(r7)               ; OUT
        move    a1,x:carrier_mod        ; the carrier reads this, masked
        move    a1,a
        move    x:(r1)+,x0              ; CONTRIB
        and     x0,a
        move    a1,a
        move    a1,x:(r3)+              ; contribution scratch
        add     a,b                     ; running sum
        endm

; The modulator is always modulated by its own feedback, and reaches the mix
; only in the additive connection.
opmodulator macro
        move    r1,r7
        move    x:(r1)+,x0              ; OUT
        move    x:(r1),a                ; PREV
        move    x0,x:(r1)+              ; PREV = OUT
        add     x0,a                    ; the two previous outputs
        move    x:(r1)+,y0              ; FBGAIN
        move    a1,x1
        mpy     x1,y0,a                 ; feedback modulation
        move    a1,y1
        move    x:(r1)+,x0              ; MODMASK, unused by the modulator
        opbody
        endm

; The carrier takes the modulator's output through its own mask, which is the
; complement of the modulator's contribution mask, and always reaches the mix.
opcarrier macro
        move    r1,r7
        move    x:(r1)+,x0              ; OUT
        move    x:(r1),a                ; PREV
        move    x0,x:(r1)+              ; PREV = OUT
        move    x:(r1)+,y0              ; FBGAIN, unused by the carrier
        move    x:(r1)+,x0              ; MODMASK
        move    x:carrier_mod,a
        and     x0,a
        move    a1,y1                   ; modulation for this operator
        opbody
        endm

; --------------------------------------------------------------- rendering
;
; One frame at a time. An exact OPL envelope advances every sample under a
; chip-wide counter, so the operator-major block loop that an approximated
; kernel can use is not available here.

render_frames:
        move    a1,x1
        tst     a
        jeq     render_done
        move    x:frame_pointer,r5
profile_start:
        do      x1,render_loop
        jsr     render_one_frame
        nop                             ; a DO loop must not end on a jump
render_loop:
profile_end:
        move    r5,x:frame_pointer
render_done:
        rts

render_one_frame:
        move    #>OP_STATE,r1
        move    #>CONTRIB_SCRATCH,r3
        move    #>SHIFT_TABLE,n2
        move    x:channel_count,a
        move    a1,x1
        clr     b                       ; running sum over every operator
        do      x1,frame_channels_done
        opmodulator
        opcarrier
frame_channels_done:
        move    b1,x:frame_sum


        ; Left and right differ from the plain sum only by the chip's output
        ; pipeline: a delayed operator contributes its previous output, so
        ; the left mix is the sum minus this frame's delayed group plus the
        ; last frame's. A two-operator channel sum never overflows sixteen
        ; bits, so this decomposition is exact rather than an approximation.
        clr     a
        move    x:delayed_count,b
        tst     b
        jeq     no_delayed
        move    b1,x1
        move    x:carrier_first,r0
        move    #>2,n0
        do      x1,delayed_done
        move    x:(r0)+n0,x0
        add     x0,a
delayed_done:
no_delayed:
        move    a1,x0                   ; this frame's delayed group
        move    x:frame_sum,a
        move    x:ldelay_prev,y0
        sub     x0,a
        add     y0,a                    ; left mix
        move    x0,x:ldelay_prev

        move    a1,x:left_mix

        ; The right mix has its own, later delay threshold: only the last
        ; three OPL3 channels' carriers cross it, and none at nine channels.
        clr     a
        move    x:rdelayed_count,b
        tst     b
        jeq     no_rdelayed
        move    b1,x1
        move    x:rcarrier_first,r0
        move    #>2,n0
        do      x1,rdelayed_done
        move    x:(r0)+n0,x0
        add     x0,a
rdelayed_done:
no_rdelayed:
        move    a1,x0
        move    x:frame_sum,a
        move    x:rdelay_prev,y0
        sub     x0,a
        add     y0,a                    ; right mix for the next frame
        move    x0,x:rdelay_prev
        move    a1,x:frame_sum          ; reuse: the right mix to emit next

        move    x:right_pending,a       ; the right mix trails by one frame
        jsr     clip16
        move    a1,x:(r5)+
        move    x:left_mix,a
        jsr     clip16
        move    a1,x:(r5)+
        move    x:frame_sum,a
        move    a1,x:right_pending
        rts

; Saturate the accumulator to signed sixteen bits, as the chip's output does.
clip16:
        move    #>32767,x0
        cmp     x0,a
        jle     clip16_low
        move    x0,a
clip16_low:
        move    #>-32768,x0
        cmp     x0,a
        jge     clip16_done
        move    x0,a
clip16_done:
        rts
