#!/usr/bin/env python3
"""
Extract HTML, CSS, and JS from include/html.h into separate web/ files

This is a one-time script to split the monolithic html.h into editable files.
Run once, then use build_web.py during builds to reassemble.
"""

import re
from pathlib import Path

def extract_html_from_header(html_h_path):
    """Extract the raw HTML string from html.h"""
    content = html_h_path.read_text(encoding='utf-8')
    match = re.search(r'R"rawliteral\((.*?)\)rawliteral"', content, re.DOTALL)
    if not match:
        raise ValueError("Could not find HTML content in html.h")
    return match.group(1)

def extract_css_from_html(html):
    """Extract CSS from <style> tags"""
    match = re.search(r'<style>(.*?)</style>', html, re.DOTALL)
    if match:
        return match.group(1).strip()
    return ""

def extract_js_from_html(html):
    """Extract JavaScript from <script> tags (not src=...)"""
    # Find all script tags without src attribute
    scripts = []
    for match in re.finditer(r'<script>(.*?)</script>', html, re.DOTALL):
        scripts.append(match.group(1).strip())
    return '\n\n'.join(scripts)

def remove_css_from_html(html):
    """Remove <style> tag and replace with link"""
    html = re.sub(r'  <style>.*?</style>\n', '  <link rel="stylesheet" href="style.css">\n', html, flags=re.DOTALL)
    return html

def remove_js_from_html(html):
    """Remove inline <script> tags and replace with src"""
    # Remove all inline scripts (not src=...)
    html = re.sub(r'  <script>\n.*?  </script>\n', '', html, flags=re.DOTALL)
    # Add script tag before </head>
    html = html.replace('</head>', '  <script src="script.js"></script>\n</head>')
    return html

def main():
    script_dir = Path(__file__).parent
    html_h_path = script_dir / 'include' / 'html.h'
    web_dir = script_dir / 'web'

    # Create web directory
    web_dir.mkdir(exist_ok=True)

    print(f"Reading HTML from: {html_h_path}")
    html = extract_html_from_header(html_h_path)

    print("Extracting CSS...")
    css = extract_css_from_html(html)
    css_path = web_dir / 'style.css'
    css_path.write_text(css, encoding='utf-8')
    print(f"  Wrote: {css_path}")

    print("Extracting JavaScript...")
    js = extract_js_from_html(html)
    js_path = web_dir / 'script.js'
    js_path.write_text(js, encoding='utf-8')
    print(f"  Wrote: {js_path}")

    print("Creating HTML file...")
    html = remove_css_from_html(html)
    html = remove_js_from_html(html)
    html_path = web_dir / 'index.html'
    html_path.write_text(html, encoding='utf-8')
    print(f"  Wrote: {html_path}")

    print("\n[OK] Web files extracted successfully!")
    print(f"\nEdit these files:")
    print(f"  - {html_path}")
    print(f"  - {css_path}")
    print(f"  - {js_path}")
    print(f"\nThey will be automatically combined during build.")

if __name__ == '__main__':
    main()
