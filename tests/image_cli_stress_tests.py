"""Exercise real one-request IPC connections, including immediate peer close.

The server's unknown-job response is deliberately small and synchronous. Old
CLI clients sporadically reported a send failure despite receiving that whole
response. No mutation is retried, and all service state stays in --output.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--service', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--requests', type=int, default=2000)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'journal').mkdir()
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())],
                                    output_roots=[str(args.output.resolve())],
                                    allow_overwrite=False,
                                    policy_revision='cli-peer-close-regression')))
    endpoint = 'fv-cli-stress-' + uuid.uuid4().hex
    count = 0
    started = time.monotonic()
    report = dict(passed=False,requested=args.requests)
    with (args.output / 'service.log').open('w', encoding='utf-8') as log:
        service = subprocess.Popen([str(args.service), '--scope', str(scope),
            '--journal', str(args.output / 'journal'), '--endpoint', endpoint],
            stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8',
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            assert json.loads(service.stdout.readline())['ready']
            for _ in range(args.requests):
                response = subprocess.run([str(args.cli), '--endpoint', endpoint],
                    input='{"version":1,"operation":"get_job","id":"does-not-exist"}\n',
                    capture_output=True, text=True, encoding='utf-8', timeout=15,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                value = json.loads(response.stdout)
                assert response.returncode == 1 and value.get('error', {}).get('code') == 'invalid_argument', (value, response.stderr)
                count += 1
            report['passed'] = True
        except Exception as exc:
            report['error'] = repr(exc)
            raise
        finally:
            report.update(completed=count,seconds=time.monotonic()-started,
                          service_exit_before_shutdown=service.poll())
            service.terminate()
            service.wait(timeout=15)
            (args.output/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
