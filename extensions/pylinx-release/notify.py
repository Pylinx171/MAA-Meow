#!/usr/bin/env python3
"""Send an Actions warning or failure summary to the configured Gmail address."""
from email.message import EmailMessage
import os
from pathlib import Path
import smtplib
import ssl
import sys


def main():
    address = os.environ.get("GMAIL_ADDRESS", "").strip()
    password = os.environ.get("GMAIL_APP_PASSWORD", "").replace(" ", "")
    if not address or not password:
        print("::error::Configure PYLINX_GMAIL_ADDRESS and PYLINX_GMAIL_APP_PASSWORD to receive build alerts")
        return 1
    run = f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
    report = Path(sys.argv[1])
    details = report.read_text(encoding="utf-8")[:16_000] if report.is_file() else ""
    build = os.environ["BUILD_RESULT"]
    publish = os.environ["PUBLISH_RESULT"]
    if os.environ.get("TEST_EMAIL") == "true":
        status = "邮件通知测试"
    elif "failure" in (build, publish):
        status = "同步、构建或发布失败"
    else:
        status = "上游接入点发生变化"
    message = EmailMessage()
    message["From"] = address
    message["To"] = address
    message["Subject"] = f"[MAA-Meow pylinx171] {status}"
    message.set_content(f"{status}\n构建 {build}，发布 {publish}\n{run}\n\n{details}")
    try:
        with smtplib.SMTP_SSL("smtp.gmail.com", 465, timeout=30, context=ssl.create_default_context()) as smtp:
            smtp.login(address, password)
            smtp.send_message(message)
    except (OSError, smtplib.SMTPException) as error:
        print(f"::error::Gmail delivery failed ({type(error).__name__}); check the Gmail Actions Secrets")
        return 1
    print("Build alert sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
