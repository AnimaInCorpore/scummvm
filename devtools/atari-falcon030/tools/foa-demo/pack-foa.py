#!/usr/bin/env python3
"""Split the practical-kernel fixture's outputs into the two blobs FOA.TOS embeds.

opl-rt-fixture writes a data image for the DSP bench, whose header is the
kernel's tables and operator records and whose tail is the bench's block
chunks; only the header is wanted here. It also writes the stream-mode
periods (--play), which are the score and are used as they are.

  foa-tables.bin  'OPLR', block count, then per block: space, address, word
                  count, the words (the fixture's header, cut after its blocks)
  foa-score.bin   'OPLP', period count, then per period: event count, two
                  words per event, a PCM flag
"""
import argparse
from pathlib import Path
import struct

TABLES_MAGIC = 0x4F504C52
SCORE_MAGIC = 0x4F504C50


def words(data, offset, count):
    return struct.unpack_from(f">{count}I", data, offset)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_image", type=Path, help="the fixture's OPLDATA image")
    parser.add_argument("play_data", type=Path, help="the fixture's --play file")
    parser.add_argument("--tables", type=Path, required=True)
    parser.add_argument("--score", type=Path, required=True)
    args = parser.parse_args()

    image = args.data_image.read_bytes()
    magic, blocks = words(image, 0, 2)
    if magic != TABLES_MAGIC:
        raise SystemExit("the data image has the wrong magic")
    at = 8
    for _ in range(blocks):
        space, address, count = words(image, at, 3)
        if space > 1:
            raise SystemExit("a data block names a space that is neither X nor Y")
        at += 12 + 4 * count
    args.tables.write_bytes(image[:at])

    score = args.play_data.read_bytes()
    magic, periods = words(score, 0, 2)
    if magic != SCORE_MAGIC:
        raise SystemExit("the play data has the wrong magic")
    # walk the periods so a truncated file cannot be embedded
    at = 8
    events = 0
    for _ in range(periods):
        (count,) = words(score, at, 1)
        events += count
        at += 4 * (1 + 2 * count)
        (pcm,) = words(score, at, 1)
        if pcm:
            raise SystemExit("the score carries PCM; FOA.TOS plays music only")
        at += 4
    if at != len(score):
        raise SystemExit("the play data is not a whole number of periods")
    args.score.write_bytes(score)
    print(f"{blocks} table blocks, {periods} periods, {events} events")


if __name__ == "__main__":
    main()
