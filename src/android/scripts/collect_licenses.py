#!/usr/bin/env python3
"""Writes the licenses of the native libraries in the APK to app/src/main/assets/licenses/native.txt.

  collect_licenses.py <vcpkg_installed/arm64-android>

The vcpkg folder is created by the CMake configure step of the app, e.g.
app/.cxx/RelWithDebInfo/<hash>/arm64-v8a/vcpkg_installed/arm64-android. Run it again when dependencies change.
"""
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
OUTPUT = os.path.join(ROOT, 'src', 'android', 'app', 'src', 'main', 'assets', 'licenses', 'native.txt')

# libraries in the dependencies folder and their license files
BUNDLED = [
    ('cubeb', 'dependencies/cubeb/LICENSE'),
    ('Dear ImGui', 'dependencies/imgui/LICENSE.txt'),
    ('ih264d', 'dependencies/ih264d/NOTICE'),
    ('libadrenotools', 'dependencies/libadrenotools/LICENSE'),
    ('Vulkan-Headers', 'dependencies/Vulkan-Headers/LICENSE.md'),
    ('xbyak_aarch64', 'dependencies/xbyak_aarch64/LICENSE'),
    ('ZArchive', 'dependencies/ZArchive/LICENSE'),
]

OTHER = [
    ('stb_image', 'stb_image is available under the MIT license or in the public domain (unlicense), '
                  'see dependencies/stb/include/stb_image.h.'),
    ('Font Awesome', 'Font Awesome Free by Fonticons, Inc. The font is licensed under the SIL Open Font License 1.1, '
                     'the icons under CC BY 4.0. https://fontawesome.com/license/free'),
    ('Noto Sans', 'The shared fonts in resources/sharedFonts are based on Noto Sans and licensed under the '
                  'SIL Open Font License 1.1. https://openfontlicense.org'),
]

SEPARATOR = '=' * 80


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read().strip()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    share = os.path.join(sys.argv[1], 'share')

    # many packages (all of boost) share the same license text
    texts = {}
    for package in sorted(os.listdir(share)):
        copyright_file = os.path.join(share, package, 'copyright')
        if os.path.isfile(copyright_file):
            # boost comes as dozens of packages
            name = 'boost' if package.startswith('boost-') else package
            names = texts.setdefault(read(copyright_file), [])
            if name not in names:
                names.append(name)
    for name, path in BUNDLED:
        texts.setdefault(read(os.path.join(ROOT, path)), []).append(name)

    sections = [(', '.join(names), text) for text, names in texts.items()]
    sections += OTHER
    sections.sort(key=lambda section: section[0].lower())

    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, 'w', encoding='utf-8') as out:
        out.write('Cidemu is licensed under the Mozilla Public License 2.0 like Cemu. It contains the following '
                  'native libraries.\n')
        for title, text in sections:
            out.write(f'\n{SEPARATOR}\n{title}\n{SEPARATOR}\n\n{text}\n')


if __name__ == '__main__':
    main()
