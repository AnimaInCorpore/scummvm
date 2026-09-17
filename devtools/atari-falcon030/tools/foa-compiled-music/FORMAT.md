# FCM1 experimental binary contract

Version 1, development profile, 2026-09-14. All integers are unsigned and
big-endian unless a field explicitly preserves an original byte layout.
No pointers, host structs or floating-point values appear on disk. This is
an evolving prototype, not a stable interchange standard.

The first real-game consumer and its narrower accepted runtime profile are
documented in [INTEGRATION.md](INTEGRATION.md). The on-disk layout is unchanged.

## Container

Header, 12 bytes: `char magic[4] = FCM1`, `u16 version = 1`, `u16 sections`,
`u32 total_file_bytes`. It is followed by `sections` directory entries:

| Field | Bytes | Meaning |
| --- | ---: | --- |
| tag | 4 | Four-character section ID |
| offset | 4 | Absolute file offset |
| length | 4 | Payload bytes, excluding directory/alignment |
| crc32 | 4 | zlib/IEEE CRC-32 of payload |

Payloads follow directory order, aligned to four bytes with zero padding;
there are no gaps beyond required alignment, overlaps or trailing bytes.
Tags are unique. `SCOR` is required. Other sections describe the explicit ROM
profile; a score-only package is legal but cannot supply a synthesizer.
Readers must validate the container, lengths and references before using
the lightweight runtime views. `runtime.h` is not the full package validator.

## SCOR: independent cues and tracks

Starts with `u32 cue_count`, then `cue_count` 12-byte entries:
`u16 sound_id, u16 room, u32 offset, u32 bytes`. Offsets are SCOR-relative.
Entries are strictly increasing by actual DSOU sound ID, not physical scan
ordinal. Cue payloads follow directory order, aligned to four bytes.

Each cue begins with four `u16` fields: original SMF format, ticks/quarter,
track count and prefix length. The prefix is the complete original 16-byte
MDhd block (`MDhd`, payload length 8, eight payload bytes). SMPTE divisions
are not supported in this profile. All 204 measured cues are SMF format 2:
their tracks are independently selectable, not simultaneous parallel tracks.

After the prefix come `track_count` eight-byte descriptors:
`u32 event_count, u32 event_offset`. All event tables then follow contiguously
in track order. Event and argument offsets are cue-relative. Each event is:

| Field | Bytes | Meaning |
| --- | ---: | --- |
| tick | 4 | Absolute source MIDI tick; nondecreasing within its track |
| opcode | 2 | Instruction below |
| argument_bytes | 2 | Decoded argument length |
| argument | 4 | Inline channel data or offset into the payload pool |

Same-tick events retain source order. Each track ends with its original
end-of-track event; no earlier end-of-track event is allowed. The variable
payload pool follows all event tables. Identical byte payloads share storage
within a cue. Payloads are not individually aligned. Empty arguments may
point one past the cue end. Channel data is right-aligned in the `u32`:
two bytes `key, velocity` become `(key << 8) | velocity`.

| Opcode | Arguments and reconstruction |
| --- | --- |
| `0080..00EF` | Original channel status; one byte for program/channel pressure, otherwise two |
| `FF00..FFFF` | Meta type in low byte, original meta payload; `FF51` is a nonzero three-byte tempo |
| `7D00..7DFF` | Supported iMUSE command in low byte, encoding described below |
| `4100` | One logical-part byte plus 246 original custom-timbre bytes |
| `F0F0`, `F0F7` | Original opaque SMF SysEx or escape-packet payload, unchanged |

Supported iMUSE commands `00,30,31,32,33,34,35,50` retain their first literal
argument byte and unpack each subsequent nibble pair into one byte. Commands
`01,02,40,51,60` retain arguments verbatim. Numbers here are hexadecimal.
Reconstruction supplies the `7D, command` prefix and terminal `F7`. Signed
values stay as original two's-complement bytes; interpretation belongs to the
existing command handler. Unknown messages are opaque, never discarded.

`4100` recognizes the full source payload
`41 part 16 12 04 00 00 [246 bytes] checksum F7`, only if its framing, length,
seven-bit range and Roland checksum validate. Reconstruction restores that
payload exactly. This is **iMUSE part instrument assignment**, not an immediate
write to a particular hardware temporary timbre. Other Roland messages stay
opaque. CDEF matching can resolve recognized definitions to normalized INST
records at load time without changing the event's timing or part ownership.

## Compiled instrument bank

Exporter profile: complete MT-32 control ROM 1.07 and MT-32 PCM ROM, normalized
through the host Munt memory interface. Munt executes no audio rendering here.

| Section | Record layout |
| --- | --- |
| `INST` | 22 bytes: original 14 common-timbre bytes plus four `u16` PARM indices |
| `PARM` | Distinct 58-byte Munt `TimbreParam::PartialParam` layouts, including muted partial data |
| `PART` | Four 16-byte descriptors per INST, in partial-slot order; fields below |
| `CDEF` | Original 246-byte custom definitions, unique in first encounter order |
| `PINI` | Eight bytes per PARM: `u16 velocity_curve, resonance_subtraction, resonance_decay, reserved=0` |
| `PVEL` | Distinct 128-byte velocity-to-pulse-width curves, unsigned bytes |
| `PTCH` | 128 eight-byte original patch records; reset defaults, not the post-iMUSE state |
| `RHYT` | 85 four-byte rhythm-temporary records, initial defaults |
| `SYST` | 23 original system bytes, initial defaults |
| `WAVE` | 128 original four-byte wave descriptors from control ROM offset `3000` hex |
| `PCML` | 262,144 big-endian 16-bit logarithmic wave-ROM words after bit permutation |
| `RLUT` | 256 big-endian `u32` ramp increment magnitudes |
| `TABL` | 2,615 bytes of model tables, offsets below |
| `PROV` | UTF-8 JSON: source-game, compiler and bank-dump SHA-256 values and profile name |

INST numbering is 0..63 factory A, 64..127 factory B, 128..157 rhythm R1..R30,
then 158+i for normalized CDEF entry i. These are compiler IDs, not MIDI
program numbers or MT-32 writable memory-bank numbers. PTCH/RHYT and current
live patch memory govern selection. In this game there are 52 CDEF entries,
210 INST records, 557 PARM records and 76 PVEL curves.

PART fields, in order: `u16 instrument`, four `u8` values `slot, kernel,
pair_mix, partner_slot`, then five `u16` values `parameter, wave, enabled,
no_sustain, reserved`. Kernel IDs: 0 square, 1 saw, 2 ROM-wave. Wave is `FFFF`
for synthetic partials. Enabled is the corresponding common partial-mask
bit; no-sustain retains the common envelope mode. Partner is `slot XOR 1`.
Pair-mix IDs retain Munt's structure dispatch: 0 normal, 1 ring plus mix,
2 ring only, 3 stereo split. These describe dispatch; they are not implemented
DSP kernels and do not alone specify all ring-slave interpolation/pan behavior.

For every PARM and velocity 0..127, PVEL stores
`clamp((velocity - 64) * (pulseWidthVeloSensitivity - 7) + pulseTable[pulseWidth], 0, 255)`.
PINI resonance values use `r = tvf.resonance + 1`, subtraction `(32-r)<<10`
and decay `decayTable[r>>2]<<2`. These remain valid only while the corresponding
parameters are unchanged. Unknown live timbre edits require recompilation of
affected descriptors/constants or an explicit unsupported-feature failure.

TABL offsets, all decimal:

| Offset | Contents |
| ---: | --- |
| 0 | `exp9[512]`, big-endian u16 |
| 1024 | `logsin9[512]`, big-endian u16 |
| 2048 | `levelToAmpSubtraction[101]`, u8 |
| 2149 | `envLogarithmicTime[256]`, u8 |
| 2405 | `masterVolToAmpSubtraction[101]`, u8 |
| 2506 | `pulseWidth100To255[101]`, u8 |
| 2607 | `resAmpDecayFactors[8]`, u8 |

The source PCM bit permutation, expressed as source-bit indices for successive
destination bits from MSB to LSB, is
`0,9,1,2,3,4,5,6,7,10,11,12,13,14,15,8`. No resampling, note rendering or
waveform fitting occurs. A renderer must retain the logarithmic interpretation,
wave descriptors, interpolation and live pitch/envelope controls.

## Host-only bank interchange

FMB1 is the intermediate exporter dump, not the runtime format. It starts
with four bytes `FMB1`, then repeated `tag[4], u32 payload_bytes, payload`
without padding. It contains TIMB (complete normalized 246-byte timbres),
CDEF, PTCH, RHYT, SYST, WAVE, PCML, RLUT and TABL. FCM compilation replaces
TIMB with INST/PARM/PART and adds PINI/PVEL. The score's custom-definition
sequence must exactly equal CDEF; a mismatched bank is rejected.

## Deliberately absent

No waveform recording, precomputed note duration, fixed sequence of game
decisions, absolute wall-clock soundtrack, synthesized attack library,
controller snapshot inferred from one playthrough, or replacement iMUSE
state machine is stored here. Current note/partial phase, envelope stages,
pitch timers, mutable instruments and reverb memory belong to the eventual
live renderer and its save-state integration.
