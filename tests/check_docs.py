#!/usr/bin/env python3
"""Check repository Markdown file links without network or build artifacts."""
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
LINK = re.compile(r'\[[^\]\n]*\]\(([^\s)]+)(?:\s+"[^"\n]*")?\)')


def main():
    paths = list(ROOT.glob('*.md'))
    for name in ('docs', 'examples', 'test_samples', '.github'):
        paths.extend((ROOT / name).rglob('*.md'))
    errors = []
    for path in paths:
        text = path.read_text()
        # Code examples may illustrate links that are not actual document links.
        text = re.sub(r'^```[^\n]*\n.*?^```\s*$', '', text, flags=re.M | re.S)
        for match in LINK.finditer(text):
            target = match.group(1).strip('<>')
            url = urlsplit(target)
            if url.scheme or url.netloc or not url.path:
                continue
            dest = ((ROOT if url.path.startswith('/') else path.parent) /
                    unquote(url.path.lstrip('/'))).resolve()
            if not dest.is_relative_to(ROOT) or not dest.exists():
                errors.append(f'{path.relative_to(ROOT)}: missing local link {target}')
    if errors:
        raise SystemExit('\n'.join(errors))
    print(f'Markdown file links passed ({len(paths)} documents)')


if __name__ == '__main__':
    main()
