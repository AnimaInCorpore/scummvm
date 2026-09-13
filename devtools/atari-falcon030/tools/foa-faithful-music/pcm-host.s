; Standalone, preloaded PCM transport gate, not a ScummVM music backend.
; Route DMA playback to DAC and DMA record to verify the digital samples.
        include "xbios.i"
        include "config.i"
        text
start:
        Locksnd
        cmpi.l  #1,d0
        bne     failed
        Buffoper #0
        Sndstatus #1
        Setmode #1
        Settracks #0,#0
        Setmontracks #0
        Soundcmd #0,#0
        Soundcmd #1,#0
        Soundcmd #4,#1
        Devconnect #0,#9,#0,#1,#1
        Setbuffer #0,#source,#source_end
        Setbuffer #1,#output,#output_end
        Supexec ticks
        move.l  d0,stats
        move.l  #$13579b,d7
        Buffoper #5
        move.w  #1200,remaining
wait:
        Vsync
        Buffoper #-1
        btst    #0,d0
        beq.s   finished
        subq.w  #1,remaining
        bne.s   wait
        Buffoper #0
        Unlocksnd
        bra     failed
finished:
        Supexec ticks
        move.l  d0,stats+4
        move.l  #$2468ac,d7
        nop
        Vsync
        Vsync
        Buffoper #0
        Unlocksnd
        Fcreate filename,#0
        tst.l   d0
        bmi     failed
        move.w  d0,handle
        Fwrite handle,#OUTPUT_BYTES,output
        cmp.l   #OUTPUT_BYTES,d0
        bne     failed
        Fclose handle
        Fcreate statsname,#0
        tst.l   d0
        bmi     failed
        move.w  d0,handle
        Fwrite handle,#8,stats
        cmpi.l  #8,d0
        bne     failed
        Fclose handle
        Pterm0
failed:
        move.w  #1,-(sp)
        move.w  #$4c,-(sp)
        trap    #1
ticks:
        move.l  $4ba,d0
        rts
        data
filename: dc.b 'OUTPUT.RAW',0
statsname: dc.b 'STATS.RAW',0
        even
source: incbin "SOURCE.RAW"
source_end:
        bss
handle: ds.w 1
remaining: ds.w 1
stats: ds.l 2
output: ds.b OUTPUT_BYTES
output_end:
        end
