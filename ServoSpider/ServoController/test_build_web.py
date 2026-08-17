#!/usr/bin/env python3
"""
Standalone test script for build_web.py functionality
Run this to test that the web build process works correctly
"""

from pathlib import Path
import re

def inline_css(html, css_content):
    """Replace <link rel="stylesheet" href="style.css"> with inline <style>"""
    style_tag = f'  <style>\n{css_content}\n  </style>'
    # Use a simple string replace instead of regex to avoid escaping issues
    html = html.replace('  <link rel="stylesheet" href="style.css">\n', style_tag + '\n')
    return html

def inline_js(html, js_content):
    """Replace <script src="script.js"></script> with inline <script>"""
    script_tag = f'  <script>\n{js_content}\n  </script>'
    # Use a simple string replace instead of regex to avoid escaping issues
    html = html.replace('  <script src="script.js"></script>\n', script_tag + '\n')
    return html

def test_build():
    """Test combining web files into html.h"""
    script_dir = Path(__file__).parent
    web_dir = script_dir / 'web'
    include_dir = script_dir / 'include'

    html_path = web_dir / 'index.html'
    css_path = web_dir / 'style.css'
    js_path = web_dir / 'script.js'
    output_path = include_dir / 'html.h'

    # Check if web files exist
    if not html_path.exists():
        print(f"Error: {html_path} not found")
        return False

    print("Building html.h from web/ sources...")
    print(f"  Reading: {html_path}")
    print(f"  Reading: {css_path}")
    print(f"  Reading: {js_path}")

    # Read source files
    html = html_path.read_text(encoding='utf-8')
    css = css_path.read_text(encoding='utf-8') if css_path.exists() else ""
    js = js_path.read_text(encoding='utf-8') if js_path.exists() else ""

    print(f"  HTML: {len(html)} chars")
    print(f"  CSS: {len(css)} chars")
    print(f"  JS: {len(js)} chars")

    # Inline CSS and JS
    if css:
        html = inline_css(html, css)
        print("  [OK] Inlined CSS")
    if js:
        html = inline_js(html, js)
        print("  [OK] Inlined JS")

    # Create header file content
    header_content = f"""// HTML page for configuration
const char* htmlPage = R"rawliteral({html})rawliteral";
"""

    # Write output
    output_path.write_text(header_content, encoding='utf-8')
    print(f"\n[OK] Generated {output_path}")
    print(f"     Total size: {len(header_content)} chars")

    return True

if __name__ == '__main__':
    try:
        success = test_build()
        if success:
            print("\n[SUCCESS] Web build test passed!")
            print("\nYou can now:")
            print("  1. Edit files in web/ directory")
            print("  2. Build with PlatformIO (build_web.py runs automatically)")
            print("  3. Or run this test script manually to regenerate html.h")
        else:
            print("\n[FAILED] Web build test failed")
    except Exception as e:
        print(f"\n[ERROR] {e}")
        import traceback
        traceback.print_exc()
