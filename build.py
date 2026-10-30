#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys

def run_cmd(cmd):
    print(f"\n▶ Выполнение: {' '.join(cmd)}")
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"\n❌ Ошибка: Команда завершилась с кодом {e.returncode}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n⏹️ Выполнение прервано пользователем.")
        sys.exit(0)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--clean", action="store_true", help="Очистить папку build.")
    parser.add_argument("-r", "--run", action="store_true", help="Запустить симулятор.")
    parser.add_argument("-b", "--board", default="native_sim", help="Целевая плата.")
    
    args = parser.parse_args()

    if sys.prefix == sys.base_prefix:
        print("⚠️  Внимание: Скрипт запущен вне виртуального окружения (venv).")
    
    west_cmd = "west"
    venv_west = os.path.join(sys.prefix, "bin", "west")
    if os.path.exists(venv_west):
        west_cmd = venv_west

    if args.clean:
        print("🧹 Очистка папки build...")
        shutil.rmtree("build", ignore_errors=True)

    print(f"🔨 Сборка проекта для платы: {args.board}...")
    run_cmd([west_cmd, "build", "-b", args.board, "."])

    print("\n✅ Сборка успешно завершена!")

    if args.run:
        print("🚀 Запуск симулятора...")
        run_cmd([west_cmd, "build", "-t", "run"])

if __name__ == "__main__":
    main()