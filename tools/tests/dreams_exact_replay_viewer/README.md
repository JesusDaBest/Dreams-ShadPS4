# Dreams exact replay viewer

This window only displays files written by the real offline Vulkan replay. It does not decode the
capture, invent geometry, draw a substitute cube, or decide that parity succeeded.

Run it after starting the replay process:

```powershell
python tools/tests/dreams_exact_replay_viewer/viewer.py D:\capture\exact-replay-output
```

The output directory must contain these four replay-generated PNGs:

- `candidate_near_a.png` and `candidate_near_b.png`: changing candidate views;
- `reference_a.png` and `reference_b.png`: fixed reference views.

The viewer loads each reference once. Reloading updates candidate images but does not silently
replace an already loaded reference.

## Status contract

The replay process atomically replaces `status.json` after completing an image set:

```json
{
  "schema": 1,
  "parity_verified": false,
  "search_running": false,
  "candidate": {
    "label": "captured-current-broken",
    "score": null,
    "settings": {}
  },
  "winner": {
    "saved": false,
    "count": 0,
    "path": null
  },
  "message": "Waiting for exact captured-camera parity"
}
```

`parity_verified` must be the JSON boolean `true`; a number or string is rejected. Until it is true,
Start Search and Save Current are disabled. The viewer never derives parity from the screenshots.
Candidate scores must be finite. A winner path must be relative to and remain inside the replay
output directory.

## Control markers

Buttons atomically write one request beneath `control/`:

- `start-search.request.json`
- `stop-search.request.json`
- `save-winner.request.json`

Writing a marker means only that a request was published. The UI continues to show the state from
`status.json`; it never claims that the replay process accepted the request. The replay process owns
marker consumption, image generation, parity verification, scoring, and winner persistence.

Pillow is used for smooth display scaling when installed. Tk's native PNG loader is the fallback.
Neither path changes the PNG files.

## Tests

```powershell
python -m unittest discover -s tools/tests/dreams_exact_replay_viewer -p "test_*.py" -v
```
