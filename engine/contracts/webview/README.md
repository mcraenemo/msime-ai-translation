# WebView messages

`messages.json` owns the message envelope, payload shape, and allowed sender windows.
Change it here, run `python3 generate.py`, and re-sync the copy under `ui-html/` in the same
commit. The generated TypeScript union and embedded C++ schema must pass `generate.py --check`.
`runtime.js` and `validator.h` implement the documented schema subset and run the same
`fixtures.json` compatibility cases. The standalone CMake test requires Boost.JSON;
the input engine itself does not acquire that dependency.

New messages carry `protocolVersion: 1`. A missing version is accepted as legacy v1
for existing skins; an explicit unknown version, unknown message, invalid payload,
or message sent from the wrong window is rejected before dispatch. This is envelope
validation, not a replacement for domain checks such as dictionary key validation,
allowed configuration values, or the host's HTTPS URL policy.

`ui-html/scripts/sync-contracts.py` materializes these bindings into
`ui-html/webview2/shared` and checks byte equality; CI runs it with `--check`, which
now compares against this directory directly rather than against a pinned submodule.
Settings imports the typed serializer and checks incoming messages. Classic candidate,
toolbar and menu pages load the same runtime from the host's `https://msime-contracts/`
virtual mapping; packaging must include `webview2/shared`.

Server-to-page candidate rendering currently uses the existing view functions;
this contract covers WebMessage actions and settings responses. Candidate view
model ownership is documented separately from transport envelopes.
