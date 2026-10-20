# ADR 0004: Recipes as JSON, run as one fused in-memory pipeline

Status: accepted (2026-10-20)

## Context
Users want multi-step actions on one slice ("resize to 2048, strip metadata, WebP 80"). Running each
step as its own file conversion would write intermediate files and lose quality at each re-encode.

## Decision
A recipe is a JSON file of steps from a fixed action registry, validated on load (unknown fields,
ranges, step order). Image steps chain `IWICBitmapSource` objects (decode, orientation, scaler,
flip-rotator, format converter) and pull pixels once into the encoder at the end. `image.encode` must
be the last step; a file action must be the only step. Built-in slices are recipes too.

## Consequences
- No intermediate files and a single lossy encode per output.
- Errors name the offending field (`steps[1].params.quality: 140 is above 100`); a broken user file
  is skipped and reported, the others still load.
- The recipe's canonical JSON is stored with each history run, so history shows what actually ran.
- Steps that cannot stream (future: video, OCR) will need their own runner; the JSON format and the
  history model stay the same.
