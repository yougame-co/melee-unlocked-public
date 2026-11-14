"""Linux must build every port test that the root CMake file registers for it."""
import ast
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def executable_targets(text):
    # Expand literal foreach variants so a missing variant cannot hide behind ${name}.
    lines = text.splitlines()
    def expand(lines, bindings):
        result, i = [], 0
        while i < len(lines):
            line = lines[i].split('#', 1)[0]
            loop = re.match(r'\s*foreach\((\w+)\s+([^)]*)\)', line)
            if loop:
                start, depth = i + 1, 1
                while depth:
                    i += 1
                    depth += bool(re.match(r'\s*foreach\(', lines[i]))
                    depth -= bool(re.match(r'\s*endforeach\(', lines[i]))
                values = loop[2].split()
                if values[:2] == ['IN', 'ITEMS']:  # foreach(v IN ITEMS a b) lists the same values
                    values = values[2:]
                for value in values:
                    result += expand(lines[start:i], {**bindings, loop[1]: value})
            else:
                for key, value in bindings.items():
                    line = line.replace('${' + key + '}', value)
                result += re.findall(r'add_executable\((port_\S+_test)\s', line)
            i += 1
        return result
    return set(expand(lines, {}))


def inline_includes(text, base):
    # Splice each included .cmake file in at its include() so targets keep their before/after return() position.
    out = []
    for line in text.splitlines():
        m = re.match(r'\s*include\(\s*"?([^")\s]+\.cmake)"?\s*\)', line.split('#', 1)[0])
        if not m:
            out.append(line)
            continue
        name = m[1].replace('${CMAKE_CURRENT_LIST_DIR}', str(base))
        path = Path(name) if Path(name).is_absolute() else ROOT / name
        out.append(inline_includes(path.read_text(), path.parent))
    return '\n'.join(out)


class LinuxTestTargets(unittest.TestCase):
    def test_all_root_executables_are_built_or_explicitly_excluded(self):
        script = ast.parse((ROOT / 'tools/build_linux.py').read_text())
        targets = set()
        for node in ast.walk(script):
            if isinstance(node, ast.AugAssign) and isinstance(node.target, ast.Name) and node.target.id == 'targets':
                targets.update(n.value for n in ast.walk(node.value)
                               if isinstance(n, ast.Constant) and isinstance(n.value, str))
        cmake = inline_includes((ROOT / 'CMakeLists.txt').read_text(), ROOT)
        before, after = cmake.split('  return()\n', 1)
        # Explicitly documented exclusion in the build script: these declarations are
        # unreachable in Linux configuration, hence ctest never registers them there.
        reason = next(n.value.value for n in script.body if isinstance(n, ast.Assign)
                      and any(isinstance(t, ast.Name) and t.id == 'LINUX_TEST_EXCLUSION_REASON' for t in n.targets))
        self.assertIn('after the root Linux return()', reason)
        excluded = executable_targets(after) - executable_targets(before)
        self.assertFalse(executable_targets(cmake) - targets - excluded,
                         'port tests missing from tools/build_linux.py targets')
        self.assertIn('port_gx_object_layout_test', targets)
        self.assertIn('port_friends_timing_test', executable_targets(cmake))  # from an included .cmake file

    def test_variant_expansion(self):
        self.assertEqual(executable_targets('foreach(v on off)\n add_executable(port_${v}_test a.cpp)\nendforeach()'),
                         {'port_on_test', 'port_off_test'})
        self.assertEqual(executable_targets('foreach(v IN ITEMS a b)\n add_executable(port_${v}_test a.cpp)\nendforeach()'),
                         {'port_a_test', 'port_b_test'})


if __name__ == '__main__':
    unittest.main()
