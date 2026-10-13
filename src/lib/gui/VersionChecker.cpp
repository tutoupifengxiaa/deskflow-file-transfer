/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "VersionChecker.h"

#include "common/Settings.h"
#include "common/VersionInfo.h"

#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <climits>

VersionChecker::VersionChecker(QObject *parent) : QObject(parent), m_network{new QNetworkAccessManager(this)}
{
  connect(m_network, &QNetworkAccessManager::finished, this, &VersionChecker::replyFinished, Qt::UniqueConnection);
}

void VersionChecker::checkLatest() const
{
  const QString url = Settings::value(Settings::Gui::UpdateCheckUrl).toString();
  qDebug("checking for updates at: %s", qPrintable(url));
  auto request = QNetworkRequest(url);
  auto userAgent = QString("%1 %2 on %3").arg(kAppName, kVersion, QSysInfo::prettyProductName());
  request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
  request.setRawHeader("X-Deskflow-Version", kVersion);
  request.setRawHeader("X-Deskflow-Language", qPrintable(QLocale::system().name()));
  m_network->get(request);
}

void VersionChecker::replyFinished(QNetworkReply *reply)
{
  reply->deleteLater();
  const auto httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply->error() != QNetworkReply::NoError) {
    qWarning("version check server error: %s", qPrintable(reply->errorString()));
    qWarning("version check server response: %s", qPrintable(QString(reply->readAll())));
    qWarning("error checking for updates, http status: %d", httpStatus);
    return;
  }

  qDebug("version check server success, http status: %d", httpStatus);

  const auto newestVersion = QString(reply->readAll()).trimmed();
  qDebug("version check response: %s", qPrintable(newestVersion));

  if (newestVersion.isEmpty()) {
    qWarning() << "version check is response is empty";
    return;
  }

  const auto comparison = compareVersions(kVersion, newestVersion);
  if (!comparison) {
    qWarning("version check response has an invalid version format");
    return;
  }
  if (*comparison > 0) {
    qWarning().noquote() //
        << QStringLiteral("current version %1 out of date, update available: %2").arg(kVersion, newestVersion);
    Q_EMIT updateFound(newestVersion);
  } else {
    qDebug().noquote() << QStringLiteral("current version %1 is upto date").arg(kVersion);
  }
}

int VersionChecker::getStageVersion(QString stage)
{
  const char *stableName = "stable";
  const char *rcName = "rc";
  const char *betaName = "beta";

  // use max int for stable so it's always the highest value.
  const int stableValue = INT_MAX;
  const int rcValue = 2;
  const int betaValue = 1;
  const int otherValue = 0;

  if (stage.isEmpty() || stage == stableName) {
    return stableValue;
  } else if (stage.startsWith(rcName, Qt::CaseInsensitive)) {
    static QRegularExpression re("\\d*", QRegularExpression::CaseInsensitiveOption);
    auto match = re.match(stage);
    if (match.hasMatch()) {
      // return the rc value plus the rc number (e.g. 2 + 1)
      // this should be ok since stable is max int.
      return rcValue + match.captured(1).toInt();
    }
  } else if (stage == betaName) {
    return betaValue;
  }

  return otherValue;
}

std::optional<int> VersionChecker::compareVersions(const QString &left, const QString &right)
{
  static const QRegularExpression versionPattern(
      "\\A[0-9]+(?:\\.[0-9]+){2,3}(?:-[A-Za-z][A-Za-z0-9.]*)?\\z"
  );
  if (left.size() > 128 || right.size() > 128 || !versionPattern.match(left).hasMatch() ||
      !versionPattern.match(right).hasMatch())
    return std::nullopt;

  QStringList leftParts = left.split("-");
  QStringList rightParts = right.split("-");

  QString leftNumber = leftParts.at(0);
  QString rightNumber = rightParts.at(0);

  QStringList leftNumberParts = leftNumber.split(".");
  QStringList rightNumberParts = rightNumber.split(".");
  for (const auto &parts : {leftNumberParts, rightNumberParts}) {
    for (const auto &part : parts) {
      bool okay = false;
      if (part.toInt(&okay) < 0 || !okay)
        return std::nullopt;
    }
  }
  if (left.compare(right) == 0)
    return 0; // versions are same.

  auto leftStagePart = leftParts.size() > 1 ? leftParts.at(1) : "";
  auto rightStagePart = rightParts.size() > 1 ? rightParts.at(1) : "";

  const int leftMajor = leftNumberParts.at(0).toInt();
  const int leftMinor = leftNumberParts.at(1).toInt();
  const int leftPatch = leftNumberParts.at(2).toInt();
  const int leftStage = getStageVersion(leftStagePart);

  const int rightMajor = rightNumberParts.at(0).toInt();
  const int rightMinor = rightNumberParts.at(1).toInt();
  const int rightPatch = rightNumberParts.at(2).toInt();
  const int rightStage = getStageVersion(rightStagePart);

  const bool rightWins =
      (rightMajor > leftMajor) || ((rightMajor >= leftMajor) && (rightMinor > leftMinor)) ||
      ((rightMajor >= leftMajor) && (rightMinor >= leftMinor) && (rightPatch > leftPatch)) ||
      ((rightMajor >= leftMajor) && (rightMinor >= leftMinor) && (rightPatch >= leftPatch) && (rightStage > leftStage));

  return rightWins ? 1 : -1;
}
