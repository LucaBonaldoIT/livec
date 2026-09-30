#!/usr/bin/env python3
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def wait_for_line(lines, predicate, timeout=12):
    deadline = time.monotonic() + timeout
    seen = []
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
        except queue.Empty:
            break
        seen.append(line)
        if predicate(line):
            return line
    raise AssertionError("Timed out waiting for output. Saw:\n" + "".join(seen))


def wait_for_value(lines, expected_value, timeout=12):
    def matches(line):
        if not line.startswith("value="):
            return False
        value_text, counter_text = line.strip().split()
        value = int(value_text.split("=", 1)[1])
        counter = int(counter_text.split("=", 1)[1])
        return value == expected_value(counter)

    return wait_for_line(lines, matches, timeout)


def reader(stream, lines):
    for line in iter(stream.readline, ""):
        lines.put(line)


def start_process(executable, root, main_source):
    process = subprocess.Popen(
        [str(executable), "--watch-root", str(root), str(main_source)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    stdout_lines = queue.Queue()
    stderr_lines = queue.Queue()
    threading.Thread(target=reader, args=(process.stdout, stdout_lines), daemon=True).start()
    threading.Thread(target=reader, args=(process.stderr, stderr_lines), daemon=True).start()
    return process, stdout_lines, stderr_lines


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
    for stream in (process.stdout, process.stderr):
        if stream is not None and not stream.closed:
            stream.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: reload_test.py path/to/livec")
    executable = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory(prefix="livec-jit-test-") as temporary:
        root = Path(temporary)
        (root / "shared.hpp").write_text("inline int shared_adjust(int value) { return value; }\n")
        (root / "main.cpp").write_text(
            "#include <cstdio>\n"
            "#include <string>\n"
            "#include <unistd.h>\n"
            "#include \"shared.hpp\"\n"
            "int counter;\n"
            "int value();\n"
            "int main() {\n"
            "    const int multiplier = 1;\n"
            "    for (;;) {\n"
            "        int current = value() * multiplier + shared_adjust(0);\n"
            "        std::printf(\"value=%d counter=%d\\n\", current, counter);\n"
            "        std::fflush(stdout);\n"
            "        ++counter;\n"
            "        sleep(1);\n"
            "    }\n"
            "}\n"
        )
        (root / "value.cpp").write_text(
            "#include <string>\n"
            "#include \"shared.hpp\"\n"
            "int offset();\n"
            "extern int counter;\n"
            "int value() { return offset() + counter + shared_adjust(0); }\n"
        )
        offset_source = root / "offset.cpp"
        offset_source.write_text("int offset() { return 3; }\n")

        process, output, logs = start_process(executable, root, root / "main.cpp")
        try:
            wait_for_line(logs, lambda line: "Program running" in line)
            assert wait_for_line(output, lambda line: line.strip() == "value=3 counter=0").strip() == "value=3 counter=0"

            # No reload function or watched file is named on the command line.
            # Editing any source file under the project root recompiles the project.
            offset_source.write_text("int offset() { return 10; }\n")
            wait_for_line(logs, lambda line: "JIT installed version 2" in line)
            updated = wait_for_value(output, lambda counter: 10 + counter)

            offset_source.write_text("int offset() { return ; }\n")
            wait_for_line(logs, lambda line: "Compile/reload failed" in line)
            after_failure = wait_for_value(output, lambda counter: 10 + counter)

            # New C++ files are discovered automatically on the next build.
            offset_source.write_text("int offset() { return 20; }\n")
            (root / "value.cpp").write_text(
                "int offset();\n"
                "int extra();\n"
                "extern int counter;\n"
                "int value() { return offset() + extra() + counter; }\n"
            )
            (root / "extra.cpp").write_text("int extra() { return 5; }\n")
            wait_for_line(logs, lambda line: "JIT installed version 3" in line)
            added = wait_for_value(output, lambda counter: 25 + counter)

            main_source = root / "main.cpp"
            main_source.write_text(main_source.read_text().replace("multiplier = 1", "multiplier = 2"))
            wait_for_line(logs, lambda line: "JIT installed version 4" in line)
            refreshed_main = wait_for_value(output, lambda counter: 2 * (25 + counter))
            print("PASS: edits to main() refresh its loop at an inserted safepoint")
            print("PASS: ORC JIT reloads functions from any project C++ file with persistent globals")
            print("PASS: invalid edits retain the previous JIT code")
        finally:
            stop_process(process)

        signature_root = root / "signature-change"
        signature_root.mkdir()
        signature_header = signature_root / "measure.hpp"
        signature_header.write_text("float measure(float);\n")
        (signature_root / "main.cpp").write_text(
            "#include <cstdio>\n"
            "#include <unistd.h>\n"
            "#include \"measure.hpp\"\n"
            "int main() {\n"
            "    for (;;) {\n"
            "        double result = measure(2.0f);\n"
            "        std::printf(\"result=%.1f\\n\", result);\n"
            "        std::fflush(stdout);\n"
            "        sleep(1);\n"
            "    }\n"
            "}\n"
        )
        measure_source = signature_root / "measure.cpp"
        measure_source.write_text("float measure(float value) { return value * 2.0f; }\n")
        process, output, logs = start_process(executable, signature_root, signature_root / "main.cpp")
        try:
            wait_for_line(logs, lambda line: "Program running" in line)
            wait_for_line(output, lambda line: line.strip() == "result=4.0")
            measure_source.write_text("double measure(float value) { return value * 10.0; }\n")
            wait_for_line(logs, lambda line: "JIT installed version 2" in line)
            old_caller = wait_for_line(output, lambda line: line.strip() == "result=4.0")
            assert old_caller.strip() == "result=4.0", old_caller

            # Keep the old call-site ABI until its declaration is edited too.
            signature_header.write_text("double measure(float);\n")
            wait_for_line(logs, lambda line: "JIT installed version 3" in line)
            changed_signature = wait_for_line(output, lambda line: line.strip() == "result=20.0")
            assert changed_signature.strip() == "result=20.0", changed_signature
            print("PASS: C++ signature changes keep old ABI callers safe and update rebuilt callers")
        finally:
            stop_process(process)

        reflection_root = root / "reflection"
        reflection_root.mkdir()
        reflection_main = reflection_root / "main.cpp"
        shutil.copy(repository / "examples/reflection/main.cpp", reflection_main)
        reflected = subprocess.run(
            [str(executable), "--watch-root", str(reflection_root),
             "--clang-arg", "-I", "--clang-arg", str(repository / "include"),
             str(reflection_main)],
            text=True,
            capture_output=True,
            timeout=20,
        )
        assert reflected.returncode == 0, reflected.stderr
        assert "function: add int (int, int) -> 42" in reflected.stdout, reflected.stdout
        assert "type: struct Player size=8 align=4" in reflected.stdout, reflected.stdout
        assert "field: health int" in reflected.stdout, reflected.stdout
        assert "field: speed float" in reflected.stdout, reflected.stdout
        assert "method: Player::damage" in reflected.stdout, reflected.stdout
        assert "virtual call: 7" in reflected.stdout, reflected.stdout
        assert "dynamic cast: 1" in reflected.stdout, reflected.stdout
        assert "caught exception: 42" in reflected.stdout, reflected.stdout
        assert "variable: score int value=42" in reflected.stdout, reflected.stdout
        assert "global object: hello from a C++ global" in reflected.stdout, reflected.stdout
        assert "overload: double (double, double) -> 4" in reflected.stdout, reflected.stdout
        assert "registry:" in reflected.stdout and "3 types" in reflected.stdout, reflected.stdout
        print("PASS: C++ reflection exposes native functions, classes, fields, methods, and globals")

        worker_root = root / "worker-loop"
        worker_root.mkdir()
        (worker_root / "main.cpp").write_text(
            "int worker();\n"
            "int main() { return worker(); }\n"
        )
        worker_source = worker_root / "worker.cpp"
        worker_source.write_text(
            "#include <cstdio>\n"
            "#include <unistd.h>\n"
            "int worker() {\n"
            "    for (;;) {\n"
            "        const int value = 3;\n"
            "        std::printf(\"worker = %d\\n\", value);\n"
            "        std::fflush(stdout);\n"
            "        sleep(1);\n"
            "    }\n"
            "}\n"
        )
        process, output, logs = start_process(executable, worker_root, worker_root / "main.cpp")
        try:
            wait_for_line(logs, lambda line: "Program running" in line)
            wait_for_line(output, lambda line: line.strip() == "worker = 3")
            worker_source.write_text(worker_source.read_text().replace("value = 3", "value = 9"))
            wait_for_line(logs, lambda line: "JIT installed version 2" in line)
            updated_worker = wait_for_line(output, lambda line: line.strip() == "worker = 9")
            assert updated_worker.strip() == "worker = 9", updated_worker
            print("PASS: edits refresh a currently executing non-main loop function")
        finally:
            stop_process(process)

        inline_root = root / "inline-main"
        inline_root.mkdir()
        inline_main = inline_root / "main.cpp"
        inline_main.write_text(
            "#include <cstdio>\n"
            "#include <unistd.h>\n"
            "int main() {\n"
            "    for (;;) {\n"
            "        const int value = 3;\n"
            "        std::printf(\"value = %d\\n\", value);\n"
            "        std::fflush(stdout);\n"
            "        sleep(1);\n"
            "    }\n"
            "}\n"
        )
        process, output, logs = start_process(executable, inline_root, inline_main)
        try:
            wait_for_line(logs, lambda line: "Program running" in line)
            wait_for_line(output, lambda line: line.strip() == "value = 3")
            changed_main = inline_main.read_text().replace(
                "const int value = 3;",
                'const int value = 9;\n        std::puts("new instruction");',
            )
            inline_main.write_text(changed_main)
            wait_for_line(logs, lambda line: "JIT installed version 2" in line)
            updated_main = wait_for_line(output, lambda line: line.strip() == "value = 9")
            assert updated_main.strip() == "value = 9", updated_main
            inserted_instruction = wait_for_line(output, lambda line: line.strip() == "new instruction")
            assert inserted_instruction.strip() == "new instruction", inserted_instruction
            print("PASS: editing the local const in main changes the live infinite loop")
        finally:
            stop_process(process)


if __name__ == "__main__":
    main()
