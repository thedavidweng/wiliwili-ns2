# Preserve upstream history and minimize platform diffs

We fork xfangfang/wiliwili rather than importing a source snapshot, keep origin, upstream and dragonfly remotes, and isolate Switch 2 changes so upstream merges stay cheap. We retain Switch 1 and desktop code that we do not promise to release. The rejected alternative was an NS2-only tree: cleaner today, but every upstream fix would become a manual port.
