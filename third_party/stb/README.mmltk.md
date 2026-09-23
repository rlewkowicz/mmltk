# stb additions

`stb_truetype.h` is unmodified stb_truetype 1.26 from
[nothings/stb commit f0569113c93ad095470c54bf34a17b36646bbbb5](https://github.com/nothings/stb/blob/f0569113c93ad095470c54bf34a17b36646bbbb5/stb_truetype.h).
Its header contains the upstream dual public-domain/MIT license. The application
uses the MIT option. `stb_truetype.cpp` enables the upstream implementation;
application caption layout and GPU painting live in backend imaging.
Only the bundled `src/frontend/iced/assets/SourceSansPro-Regular.otf` is loaded,
embedded at build time; its existing license remains beside the font asset.
