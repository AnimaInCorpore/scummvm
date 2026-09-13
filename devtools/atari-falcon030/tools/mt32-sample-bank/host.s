; A standalone render-to-RAM gate, stock 68030, real DSP host port.
; PCM remains in ST RAM; cursors wrap in the real bank, no pre-expanded blocks.
; Profile includes cursor/loop management, sample reads, port waits, pan/mix,
; output download and stores. Disk I/O and boot are outside the interval.
        include "xbios.i"
        include "image.i"
        include "config.i"
        text
start:
        Locksnd
        cmpi.l  #1,d0
        bne     failed
        Dsp_ExecBoot mix_boot,#MIX_BOOT_WORDS,#3
        Supexec render
        Dsp_Unlock
        Unlocksnd
        Fcreate filename,#0
        tst.l   d0
        bmi     failed
        move.w  d0,handle
        Fwrite handle,#OUTPUT_BYTES,output
        cmp.l   #OUTPUT_BYTES,d0
        bne     failed
        Fclose handle
        Pterm0
failed:
        move.w  #1,-(sp)
        move.w  #$4c,-(sp)
        trap    #1

render:
        movem.l d2-d7/a2-a6,-(sp)
        lea     $ffffa202,a5
        lea     $ffffa204,a6
        lea     output,a4
        move.w  #BLOCKS-1,d6
        move.l  #$13579b,d7          ; debugger starts CPU/DSP profiles here
period:
        lea     voices,a3
        move.w  #VOICES-1,d5
voice:
        move.l  (a3)+,a0            ; current cursor
        move.l  (a3)+,a1            ; loop start
        move.l  (a3)+,a2            ; end
gain_left:
        btst    #1,(a5)
        beq.s   gain_left
        move.l  (a3)+,(a6)
gain_right:
        btst    #1,(a5)
        beq.s   gain_right
        move.l  (a3)+,(a6)
        ifd     CHUNKED
        move.l  #INPUT_WORDS,d3
chunk:
        move.l  a2,d4
        sub.l   a0,d4
        lsr.l   #1,d4
        ifd     PACKED12
        lsr.l   #1,d4
        endc
        cmp.l   d3,d4
        bls.s   chunk_size
        move.l  d3,d4
chunk_size:
        sub.l   d4,d3
        subq.w  #1,d4
        else
        move.w  #INPUT_WORDS-1,d4
        endc
sample:
        ifnd    PACKED12
        move.w  (a0)+,d0
        ext.l   d0
        endc
wait_send:
        btst    #1,(a5)
        beq.s   wait_send
        ifd     PACKED12
        move.l  (a0)+,(a6)
        else
        move.l  d0,(a6)
        endc
        ifnd    CHUNKED
        cmpa.l  a2,a0
        blo.s   no_wrap
        move.l  a1,a0
no_wrap:
        endc
        dbra    d4,sample
        ifd     CHUNKED
        cmpa.l  a2,a0
        blo.s   chunk_no_wrap
        move.l  a1,a0
chunk_no_wrap:
        tst.w   d3
        bne.s   chunk
        endc
        move.l  a0,-20(a3)
        dbra    d5,voice
        move.w  #1023,d4
receive:
        btst    #0,(a5)
        beq.s   receive
        move.l  (a6),d0
        move.w  d0,(a4)+
        dbra    d4,receive
        dbra    d6,period
        move.l  #$2468ac,d7          ; debugger stops profiles before GEMDOS
        nop
        movem.l (sp)+,d2-d7/a2-a6
        rts
        data
filename: dc.b 'OUTPUT.RAW',0
        even
        include "bank.i"
        bss
handle: ds.w 1
output: ds.b OUTPUT_BYTES
        end
