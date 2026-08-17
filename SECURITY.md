# Security policy

## Reporting a problem

Do not open a public issue for a security problem.

Use GitHub's private vulnerability reporting: go to the repository's
**Security** tab and choose **Report a vulnerability**. That opens a private
advisory that only the maintainer can see, and it lets a fix be prepared before
anything is public.

If that option is not showing on the Security tab, email sudoarkman@gmail.com
instead. Private reporting is a setting the repository owner has to turn on, and
a report should never be lost because it was switched off.

Useful things to include, in rough order of how much they help:

- What an attacker gets. Reading a file outside the project, writing one,
  running a program, a crash you can steer.
- A file that reproduces it. A `.pbo`, a `.sdzn`, a `config.cpp`, a `.c`
  script. If the file is large, the smallest one that still works is worth more
  than the original.
- The application version, from **Help** or `DAYZSUDONodeMod.exe --version`.
- Whether it needs a real DayZ installation, a mounted `P:` drive, or DayZ
  Tools present.

Expect an acknowledgement within a week. If a fix is warranted you will be
credited in `CHANGELOG.md` unless you ask not to be.

## Supported versions

This is pre-1.0 software. Only the most recent release gets fixes. There are no
maintenance branches.

## What the attack surface actually is

Being honest about this is more useful than a list of things the project does
not do. Almost all of this application is about files and processes on the
machine it runs on. It makes exactly one kind of network request, described
first below so it is not buried.

### The update check

`src/update.cpp` asks the GitHub releases API whether a newer version has been
published. What it is and what it is not:

- It contacts nothing until the user has said it may. The answer starts as "not
  asked" and is remembered, so an install where nobody answers never sends a
  request.
- One endpoint, over HTTPS:
  `https://api.github.com/repos/<owner>/<repo>/releases?per_page=20`.
- Redirects follow Qt's `NoLessSafeRedirectPolicy`, so a redirect cannot
  downgrade the request to plain HTTP.
- It reads a version, a changelog entry and a download URL out of the JSON and
  shows them. It does not download the release and does not install anything.
  Updating is the user opening the link themselves.

Treat the reply as untrusted input, because it is: it is JSON from a remote
server, parsed by code in this repository. A version string or changelog text
that gets the parser to read out of bounds, allocate without bound, or put
attacker-chosen text somewhere it is treated as more than text, is a report
worth making. So is anything that makes the check fire before the user has
agreed to it, or that sends anything about the user's machine or projects along
with the request.

If a release ships without `tls/qschannelbackend.dll` the HTTPS request fails at
the handshake, which reads as "no update available". That is a packaging
mistake rather than a vulnerability, and `RELEASING.md` guards against it.

### PBO archives

This is the surface that matters most, because a `.pbo` is a file a user
downloads from the Steam Workshop or is handed by somebody else, and the
application reads it directly rather than shelling out to DayZ Tools.

`src/pbo/pboreader.cpp` is written against a corpus that is genuinely hostile,
measured rather than assumed. Across the installed mods on the development
machine: 22,956 entry names containing `..`, 578,059 that look absolute,
1,034,830 carrying control or reserved characters, one archive with a 150 MB
header claiming 1,298,973 entries, and one archive of 2.1 GB.

What the reader does about it:

- Entry names are sanitised before anything opens a file. Absolute paths, drive
  letters, parent directory segments, control characters, characters Windows
  reserves, names Windows reserves for devices, and names Windows would
  silently rename are each refused by name, and the refusal is reported rather
  than swallowed.
- The entry count is bounded by `fileSize / 21` rather than a constant, because
  three real mods carry roughly a million entries each and a constant would
  either refuse them or fail to bound the 150 MB header.
- Decompression output is bounded by `dataSize * 9 + 64`, the ceiling of what
  the archive's LZSS variant can expand to, so a hostile size field cannot force
  a large allocation.
- Every decompressed stream is checked against the 4 byte sum trailer that
  follows it. A wrong decode is refused rather than returned.

If you can get this reader to write a file outside the directory it was given,
allocate without bound, read out of bounds, or loop forever, that is a report
worth making.

### Project files

A `.sdzn` project file is JSON, and it is a file anyone can hand you. The mod
browser makes opening other people's work an ordinary thing to do, which makes
this a real path rather than a theoretical one.

The case already guarded: a node's trivia fields (the comments and blank lines
the graph preserves) are validated where a `.sdzn` is read and again where a
`.c` file is written, not only where they are captured. A trivia field holding
real script would otherwise be emitted into the user's mod as code, which is
code execution by a slower route: the user compiles it themselves.

Graphs opened through the mod browser are marked read only, and one predicate
on the graph's own mark answers for that, checked at three gates including
inside the only function in the application that turns a graph into a `.c`
file. A project file claiming a different origin does not get past it, because
the question asked is about the graph rather than about what the file says
about itself.

Report anything that gets attacker-controlled text into a generated script
without the user typing it, or that gets a write outside the project.

### Config files

`src/config/configtree.cpp` parses `config.cpp` files out of mods and writes
them back, reusing source text whose meaning has not changed. It runs over
files from any mod on the machine.

### External programs and the work drive

The **Run** path does more than read files, so it is worth stating plainly what
it can do:

- Creates and removes directory junctions on the `P:` work drive, through
  `cmd /c mklink`.
- Runs `taskkill`.
- Runs DayZ Tools' AddonBuilder to pack a PBO.
- Launches `DayZDiag_x64.exe` as a server and as a client.
- Rewrites `project.cfg` one line at a time, keeping every other byte.

Paths to DayZ and DayZ Tools are found through the Windows registry, Steam
library folders and a settings file, in that order. All of these are user
actions started from the interface, none of them run on opening a file, and the
application asks for no elevation. A report that turns opening a project or a
mod into any of the above is a real finding.

### What is not hardened, and is not claimed to be

- There is no sandbox. The application runs with the user's own privileges and
  can write anywhere the user can.
- Generated Enforce Script is not sandboxed either. The application writes
  script into a mod folder and the user compiles it. It is a code generator, so
  that is the job.
- The mod template under `resources/mod-template/` includes `.bat` and `.ps1`
  scripts from upstream. The application copies them into a scaffolded mod; it
  does not run them. Running them is the user's own decision, the same as with
  any project template.
