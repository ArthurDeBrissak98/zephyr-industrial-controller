"""Проверка ссылок матрицы требований на код и существующие тесты."""
import ast
from pathlib import Path
import re
import sys

import yaml

ROOT = Path(__file__).resolve().parents[1]
TAG = r"REQ-[A-Z]+-\d{2}"


def read(relative):
    path = (ROOT / relative).resolve()
    if not path.is_relative_to(ROOT) or not path.is_file():
        raise ValueError(f"Нет файла проекта: {relative}")
    return path.read_text()


def check():
    data = yaml.safe_load(read("docs/traceability/requirements.yaml"))
    if data.get("version") != 1 or not data.get("requirements"):
        raise ValueError("Нужна непустая матрица version: 1")
    ids = set()
    for row in data["requirements"]:
        id = row["id"]
        if not re.fullmatch(TAG, id) or id in ids:
            raise ValueError(f"Некорректный или повторный ID: {id}")
        ids.add(id)
        if not row.get("requirement") or not row.get("code") or not row.get("verification"):
            raise ValueError(f"{id}: нужны требование, реализация и проверка")
        if row.get("origin") not in {"project", "acs880_analogy"}:
            raise ValueError(f"{id}: неизвестный источник")
        if row["origin"] == "acs880_analogy" and not row.get("acs880_topic"):
            raise ValueError(f"{id}: нужна тема ACS880")
        for code in row["code"]:
            text = read(code["file"])
            marker = r"//\s*" + re.escape(id) + r"\s*\n"
            function = r"(?:static\s+)?(?:void|int|bool|uint8_t|struct signal_value|struct motor_parameters)\s+" + re.escape(code["symbol"]) + r"\s*\("
            if not re.search(marker + r"(?://\s*" + TAG + r"\s*\n)*" + function, text):
                raise ValueError(f"{id}: тег не найден перед {code['file']}:{code['symbol']}")
        for test in row["verification"]:
            text = read(test["file"])
            if test["kind"] == "ztest":
                expression = r"ZTEST\s*\(\s*" + re.escape(test["suite"]) + r"\s*,\s*" + re.escape(test["name"]) + r"\s*\)"
                found = re.search(expression, text)
            elif test["kind"] == "scenario":
                scenarios = yaml.safe_load(text)["scenarios"]
                found = any(s["name"] == test["name"] for s in scenarios)
            elif test["kind"] == "python":
                found = any(isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == test["name"]
                            for n in ast.parse(text).body)
            else:
                raise ValueError(f"{id}: неизвестный тип проверки {test['kind']}")
            if not found:
                raise ValueError(f"{id}: нет проверки {test['file']}:{test['name']}")
    tags = set()
    for path in (ROOT / "src").glob("*.c"):
        tags.update(re.findall(r"//\s*(" + TAG + r")\b", path.read_text()))
    if tags != ids:
        raise ValueError(f"Теги и матрица расходятся: {sorted(tags ^ ids)}")
    print(f"PASS: {len(ids)} требований связаны с кодом и проверками")


if __name__ == "__main__":
    try:
        check()
    except (ValueError, KeyError, TypeError, OSError, yaml.YAMLError, SyntaxError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
