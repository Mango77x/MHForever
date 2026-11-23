# Pokke Lab

Open index.html in a current Chromium browser. Select the two directories directly containing gl_*.ppm captures and click Compare captures. The first differing filename is selected automatically. Browse the two images, blend equal-sized frames, and export a Markdown report. Processing takes place locally. A missing filename is reported separately. Matching filenames cannot establish synchronized simulation time.

The command line uses the same RGB comparison on Windows or Linux with Node.js 18 or later:

    node cli.js baseline candidate > comparison.md

Exit codes: 0 identical, 1 differences, 2 input error. Input is binary P6 RGB with maxval 255. Pixel bounds are inclusive, starting at the top left.

To build the optional Windows launcher from this directory in PowerShell:

    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /out:PokkeLab.exe /resource:index.html,index.html /resource:compare.js,compare.js Launcher.cs

The executable opens the embedded interface in the default browser and leaves its two interface files in the temporary PokkeLab directory.
