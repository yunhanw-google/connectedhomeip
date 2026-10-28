# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Hermetic Unit Tests for Cirque and ConnectedHomeIP Agent Skills.

Programmatically verifies:
1. Both Cirque repository skill (`cirque-virtual-home/SKILL.md`) and
   ConnectedHomeIP repository skill (`cirque-virtual-testing/SKILL.md`) exist and
   contain valid YAML frontmatter ('name', 'description').
2. Every single tool registered by CirqueMcpServer (14 tools) is documented in
   `cirque-virtual-home/SKILL.md`.
3. Every single tool registered by `matter_cirque_extension.py` (7 tools) is
   documented in `cirque-virtual-testing/SKILL.md`.
4. Every relative file path cited in both `SKILL.md` files resolves to an
   existing file on disk.
5. Repository skill catalogs (`.agents/skills.md`) properly index both skills.
"""

import os
import re
import sys
import unittest

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CHIP_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '..', '..', '..', '..'))
CIRQUE_ROOT = os.path.join(CHIP_ROOT, 'third_party', 'cirque', 'repo')

if CIRQUE_ROOT not in sys.path:
  sys.path.insert(0, CIRQUE_ROOT)
if SCRIPT_DIR not in sys.path:
  sys.path.insert(0, SCRIPT_DIR)

from cirque.mcp.server import CirqueMcpServer
import matter_cirque_extension as matter_ext


def parse_yaml_frontmatter(file_content: str) -> dict:
  """Extracts simple key-value pairs from YAML frontmatter between --- markers."""
  match = re.match(r'^---\s*\n(.*?)\n---\s*\n', file_content, re.DOTALL)
  if not match:
    return {}
  yaml_block = match.group(1)
  data = {}
  current_key = None
  current_val = []

  for line in yaml_block.splitlines():
    key_match = re.match(r'^([a-zA-Z0-9_-]+):\s*(.*)$', line)
    if key_match:
      if current_key:
        data[current_key] = ' '.join(current_val).strip()
      current_key = key_match.group(1)
      val = key_match.group(2).strip()
      current_val = [val] if val else []
    elif current_key and line.startswith('    '):
      current_val.append(line.strip())

  if current_key:
    data[current_key] = ' '.join(current_val).strip()
  return data


def extract_backticked_relative_paths(file_content: str) -> list:
  """Finds backticked candidate relative file paths in markdown."""
  raw_tokens = re.findall(r'`([^`\n]+)`', file_content)
  candidate_paths = []
  for token in raw_tokens:
    token = token.strip()
    # Filter out commands, shell options, URLs, code snippets, etc.
    if any(token.startswith(prefix) for prefix in ('http://', 'https://', 'cirque://', '-', '--', '$', '{', '<')):
      continue
    if ' ' in token or '(' in token or ')' in token or '=' in token or '|' in token or '*' in token:
      continue
    # Candidates must have a path separator or known file extension
    if '/' in token or any(token.endswith(ext) for ext in ('.py', '.md', '.sh', '.c', '.cpp', '.h', '.gn', '.yaml', '.txt')):
      candidate_paths.append(token)
  return candidate_paths


class TestCirqueAgentSkills(unittest.TestCase):
  """Unit tests for agent skills documentation and registration conformance."""

  def setUp(self):
    self.cirque_skill_path = os.path.join(
        CIRQUE_ROOT, '.agents', 'skills', 'cirque-virtual-home', 'SKILL.md'
    )
    self.chip_skill_path = os.path.join(
        CHIP_ROOT, '.agents', 'skills', 'cirque-virtual-testing', 'SKILL.md'
    )

  def test_cirque_virtual_home_skill_frontmatter(self):
    """Verifies cirque-virtual-home/SKILL.md exists with valid YAML frontmatter."""
    self.assertTrue(
        os.path.isfile(self.cirque_skill_path),
        f'Missing cirque-virtual-home SKILL.md at {self.cirque_skill_path}',
    )
    with open(self.cirque_skill_path, 'r', encoding='utf-8') as f:
      content = f.read()

    meta = parse_yaml_frontmatter(content)
    self.assertEqual(meta.get('name'), 'cirque-virtual-home')
    description = meta.get('description', '')
    self.assertTrue(len(description) > 30, 'description must not be empty')
    self.assertIn('Use when', description)
    self.assertIn("Don't use", description)

  def test_cirque_virtual_testing_skill_frontmatter(self):
    """Verifies cirque-virtual-testing/SKILL.md exists with valid YAML frontmatter."""
    self.assertTrue(
        os.path.isfile(self.chip_skill_path),
        f'Missing cirque-virtual-testing SKILL.md at {self.chip_skill_path}',
    )
    with open(self.chip_skill_path, 'r', encoding='utf-8') as f:
      content = f.read()

    meta = parse_yaml_frontmatter(content)
    self.assertEqual(meta.get('name'), 'cirque-virtual-testing')
    description = meta.get('description', '')
    self.assertTrue(len(description) > 30, 'description must not be empty')
    self.assertIn('Use when', description)
    self.assertIn("Don't use", description)

  def test_cirque_base_tools_documented(self):
    """Verifies all 14 base Cirque tools from CirqueMcpServer are documented."""
    server = CirqueMcpServer()
    tools_resp = server.handle_message({
        'jsonrpc': '2.0',
        'id': 'test_list_tools',
        'method': 'tools/list',
        'params': {},
    })
    tools = tools_resp.get('result', {}).get('tools', [])
    self.assertEqual(len(tools), 14, f'Expected 14 base tools, found {len(tools)}')

    with open(self.cirque_skill_path, 'r', encoding='utf-8') as f:
      content = f.read()

    for tool in tools:
      tool_name = tool['name']
      self.assertIn(
          f'`{tool_name}`',
          content,
          f'Cirque tool {tool_name} is not documented in cirque-virtual-home/SKILL.md',
      )

  def test_matter_extension_tools_documented(self):
    """Verifies all 7 Matter extension tools are documented in cirque-virtual-testing."""
    server = CirqueMcpServer()
    matter_ext.register_cirque_extension(server)
    tools_resp = server.handle_message({
        'jsonrpc': '2.0',
        'id': 'test_list_ext_tools',
        'method': 'tools/list',
        'params': {},
    })
    all_tools = tools_resp.get('result', {}).get('tools', [])
    matter_tools = [
        t['name'] for t in all_tools if t['name'].startswith('matter_')
    ]
    self.assertEqual(
        len(matter_tools),
        7,
        f'Expected 7 Matter tools, found {len(matter_tools)}: {matter_tools}',
    )

    with open(self.chip_skill_path, 'r', encoding='utf-8') as f:
      content = f.read()

    for tool_name in matter_tools:
      self.assertIn(
          f'`{tool_name}`',
          content,
          f'Matter tool {tool_name} is not documented in cirque-virtual-testing/SKILL.md',
      )

  def test_cirque_virtual_home_referenced_paths_exist(self):
    """Verifies every relative file path cited in cirque-virtual-home resolves on disk."""
    with open(self.cirque_skill_path, 'r', encoding='utf-8') as f:
      content = f.read()

    paths = extract_backticked_relative_paths(content)
    # Exclude system paths or dummy example paths
    ignore = {
        '/tmp/bt_pcaps',
        '/dev/bluetooth0',
        '/dev/ttyS1',
        '/dev/kvm',
        'cirque_tap0',
        'org.bluez',
        'fi.w1.wpa_supplicant1',
        'thread_network_key.txt',
    }
    for p in paths:
      if p in ignore or p.startswith('/'):
        continue
      resolved = os.path.join(CIRQUE_ROOT, p)
      self.assertTrue(
          os.path.exists(resolved),
          f'Relative path `{p}` cited in cirque-virtual-home/SKILL.md does not exist on disk ({resolved})',
      )

  def test_cirque_virtual_testing_referenced_paths_exist(self):
    """Verifies every relative file path cited in cirque-virtual-testing resolves on disk."""
    with open(self.chip_skill_path, 'r', encoding='utf-8') as f:
      content = f.read()

    paths = extract_backticked_relative_paths(content)
    ignore = {
        'cirque_flow_visualization.html',
        'cirque_flow_trace.json',
        'cirque_tap0',
        'matter_session_keys.txt',
    }
    for p in paths:
      if p in ignore or p.startswith('/'):
        continue
      resolved = os.path.join(CHIP_ROOT, p)
      self.assertTrue(
          os.path.exists(resolved),
          f'Relative path `{p}` cited in cirque-virtual-testing/SKILL.md does not exist on disk ({resolved})',
      )

  def test_skills_catalogs_contain_skills(self):
    """Verifies that .agents/skills.md in both repositories index the skills."""
    # Cirque repo catalog
    cirque_catalog = os.path.join(CIRQUE_ROOT, '.agents', 'skills.md')
    self.assertTrue(os.path.isfile(cirque_catalog))
    with open(cirque_catalog, 'r', encoding='utf-8') as f:
      cirque_cat_text = f.read()
    self.assertIn('cirque-virtual-home', cirque_cat_text)

    # ConnectedHomeIP catalog
    chip_catalog = os.path.join(CHIP_ROOT, '.agents', 'skills.md')
    self.assertTrue(os.path.isfile(chip_catalog))
    with open(chip_catalog, 'r', encoding='utf-8') as f:
      chip_cat_text = f.read()
    self.assertIn('cirque-virtual-testing', chip_cat_text)


if __name__ == '__main__':
  unittest.main()
