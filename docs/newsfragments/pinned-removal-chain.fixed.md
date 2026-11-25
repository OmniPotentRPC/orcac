Session cleanup no longer follows `..` to check where it is. The walk
pins a descriptor for every level from the session root down, each
opened from its pinned parent with `O_DIRECTORY | O_NOFOLLOW`. Every
open, stat and unlink is made relative to one of those descriptors.
Before each descent and each unlink, every level is checked to be still
the entry that its pinned parent holds under its name. Before, `..`
was looked up again on every check. An ancestor renamed in the window
between that check and the unlink could therefore be judged by
directories reached through that lookup rather than by the ones the
walk had opened.
