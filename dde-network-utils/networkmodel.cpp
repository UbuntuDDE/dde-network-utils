/*
 * Copyright (C) 2011 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     sbw <sbw@sbw.so>
 *             kirigaya <kirigaya@mkacg.com>
 *             Hualet <mr.asianwang@gmail.com>
 *
 * Maintainer: sbw <sbw@sbw.so>
 *             kirigaya <kirigaya@mkacg.com>
 *             Hualet <mr.asianwang@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "networkmodel.h"
#include "networkdevice.h"
#include "wirelessdevice.h"
#include "wireddevice.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

using namespace dde::network;

#define CONNECTED  2

Connectivity NetworkModel::m_Connectivity(Connectivity::Full);
QStringList NetworkModel::m_deviceInterface;

NetworkDevice::DeviceType parseDeviceType(const QString &type)
{
    if (type == "wireless") {
        return NetworkDevice::Wireless;
    }
    if (type == "wired") {
        return NetworkDevice::Wired;
    }

    return NetworkDevice::None;
}

NetworkModel::NetworkModel(QObject *parent)
    : QObject(parent)
    , m_lastSecretDevice(nullptr)
    , m_connectivityChecker(new ConnectivityChecker)
    , m_connectivityCheckThread(new QThread(this))
{
    connect(this, &NetworkModel::needCheckConnectivitySecondary,
            m_connectivityChecker, &ConnectivityChecker::startCheck);
    connect(m_connectivityChecker, &ConnectivityChecker::checkFinished,
            this, &NetworkModel::onConnectivitySecondaryCheckFinished);

    m_connectivityChecker->moveToThread(m_connectivityCheckThread);
}

NetworkModel::~NetworkModel()
{
    qDeleteAll(m_devices);
    qDebug() << "quit thread";
    m_connectivityCheckThread->quit();
    m_connectivityCheckThread->wait();
}

const QString NetworkModel::connectionUuidByPath(const QString &connPath) const
{
    return connectionByPath(connPath).value("Uuid").toString();
}

const QString NetworkModel::connectionNameByPath(const QString &connPath) const
{
    return connectionByPath(connPath).value("Id").toString();
}

const QJsonObject NetworkModel::connectionByPath(const QString &connPath) const
{
    for (const auto &list : m_connections)
    {
        for (const auto &cfg : list)
        {
            if (cfg.value("Path").toString() == connPath)
                return cfg;
        }
    }

    return QJsonObject();
}

const QJsonObject NetworkModel::activeConnObjectByUuid(const QString &uuid) const
{
    for (const auto &info : m_activeConns)
    {
        if (info.value("Uuid").toString() == uuid)
            return info;
    }

    return QJsonObject();
}

const QString NetworkModel::connectionUuidByApInfo(const QJsonObject &apInfo) const
{
    for (const auto &list : m_connections)
    {
        for (const auto &cfg : list)
        {
            if (cfg.value("Ssid").toString() == apInfo.value("Ssid").toString())
                return cfg.value("Uuid").toString();
        }
    }

    return QString();
}

const QString NetworkModel::activeConnUuidByInfo(const QString &devPath, const QString &id) const
{
    for (const auto &info : m_activeConns)
    {
        if (info.value("Id").toString() != id)
            continue;

        if (info.value("Devices").toArray().contains(devPath))
            return info.value("Uuid").toString();
    }

    return QString();
}

const QJsonObject NetworkModel::connectionByUuid(const QString &uuid) const
{
    for (const auto &list : m_connections)
    {
        for (const auto &cfg : list)
        {
            if (cfg.value("Uuid").toString() == uuid)
                return cfg;
        }
    }

    return QJsonObject();
}

void NetworkModel::onActivateAccessPointDone(const QString &devPath, const QString &apPath, const QString &uuid, const QDBusObjectPath path)
{
    for (auto const dev : m_devices)
    {
        if (dev->type() != NetworkDevice::Wireless || dev->path() != devPath)
            continue;

        if (path.path().isEmpty()) {
            Q_EMIT static_cast<WirelessDevice *>(dev)->activateAccessPointFailed(apPath, uuid);
            return;
        }
    }
}

void NetworkModel::onVPNEnabledChanged(const bool enabled)
{
    if (m_vpnEnabled != enabled)
    {
        m_vpnEnabled = enabled;

        Q_EMIT vpnEnabledChanged(m_vpnEnabled);
    }
}

void NetworkModel::onProxiesChanged(const QString &type, const QString &url, const uint port)
{
    const ProxyConfig config = { port, type, url, "", "" };
    const ProxyConfig old = m_proxies[type];


    if (old.url != config.url || old.port != config.port)
    {
        m_proxies[type] = config;

        Q_EMIT proxyChanged(type, config);
    }
}

void NetworkModel::onAutoProxyChanged(const QString &proxy)
{
    if (m_autoProxy != proxy)
    {
        m_autoProxy = proxy;

        Q_EMIT autoProxyChanged(m_autoProxy);
    }
}

void NetworkModel::onProxyMethodChanged(const QString &proxyMethod)
{
    if (m_proxyMethod != proxyMethod)
    {
        m_proxyMethod = proxyMethod;

        Q_EMIT proxyMethodChanged(m_proxyMethod);
    }
}

void NetworkModel::onProxyIgnoreHostsChanged(const QString &hosts)
{
    if (hosts != m_proxyIgnoreHosts)
    {
        m_proxyIgnoreHosts = hosts;

        Q_EMIT proxyIgnoreHostsChanged(m_proxyIgnoreHosts);
    }
}

void NetworkModel::onDevicesChanged(const QString &devices)
{
    const QJsonObject data = QJsonDocument::fromJson(devices.toUtf8()).object();

    QSet<QString> devSet;

    bool changed = false;

    m_deviceInterface.clear();

    for (auto it(data.constBegin()); it != data.constEnd(); ++it) {
        const auto type = parseDeviceType(it.key());
        const auto list = it.value().toArray();

        if (type == NetworkDevice::None)
            continue;

        for (auto const &l : list)
        {
            const auto info = l.toObject();
            const QString path = info.value("Path").toString();
            bool managed = info.value("Managed").toBool();
            QString interface = info.value("Interface").toString();

            if (!m_deviceInterface.contains(interface))
                m_deviceInterface << interface;

            if (!managed) {
                qDebug() << "device: " << path << "ignored due to unmanged";
                continue;
            }

            // ???????????????InterfaceFlags??????????????????????????????
            if (type != NetworkDevice::Wireless) {
                if (!info.value("InterfaceFlags").isUndefined()) {
                    int flag = info.value("InterfaceFlags").toInt();
                    if (!(flag & NM_DEVICE_INTERFACE_FLAG_UP)) {
                        continue;
                    }
                }
            }

            if (!devSet.contains(path)) {
                devSet << path;
            }

            NetworkDevice *d = device(path);
            if (d == nullptr)
            {
                changed = true;

                switch (type)
                {
                    case NetworkDevice::Wireless: d = new WirelessDevice(info, this); break;
                    case NetworkDevice::Wired:    d = new WiredDevice(info, this);    break;
                    default:;
                }

                m_devices.append(d);

                if (d != nullptr) {
                    // init device enabled status
                    Q_EMIT requestDeviceStatus(d->path());
                }
            } else {
                d->updateDeviceInfo(info);
            }
        }
    }

    // remove unexist device
    QList<NetworkDevice *> removeList;
    for (auto const d : m_devices)
    {
        if (!devSet.contains(d->path()))
            removeList << d;
    }

    for (auto const r : removeList) {
        m_devices.removeOne(r);
        r->deleteLater();
    }

    if (!removeList.isEmpty()) {
        changed = true;
    }

//    qDeleteAll(removeList);

    if (changed) {
        Q_EMIT deviceListChanged(m_devices);
    }
}

void NetworkModel::onConnectionListChanged(const QString &conns)
{
    // m_connections ?????????????????? NetworkManager ???????????? connection
    // m_connections ?????????????????????????????????(wired,wireless,vpn,pppoe,etc.), ???????????????????????????????????? list ????????? map

    // commonConnections ???????????? m_connection ??????, ??? commenConnection ????????? "HwAddress" ?????????????????????,
    // ???????????????????????? "HwAddress" ?????? ?????????????????????????????????, ??????:
    // "HwAddress" ??????????????????????????????????????????????????????, ???????????????????????????????????? "HwAddress" ???????????????, ????????????????????????????????????

    // deviceConnections ????????????????????? "HwAddress" ????????????, ????????? map ????????? map
    // ?????? map ??????????????? m_connection ??????
    // ????????? deviceConnections ??????????????????????????????????????????, ????????????????????????????????????

    QMap<QString, QList<QJsonObject>> commonConnections;
    QMap<QString, QMap<QString, QList<QJsonObject>>> deviceConnections;

    QMap<QString, QList<QJsonObject>> wiredCommonConnections;
    QMap<QString, QMap<QString, QList<QJsonObject>>> wiredDeviceConnections;

    // ??????????????? connection
    const QJsonObject connsObject = QJsonDocument::fromJson(conns.toUtf8()).object();
    for (auto it(connsObject.constBegin()); it != connsObject.constEnd(); ++it) {
        const auto &connList = it.value().toArray();
        const auto &connType = it.key();
        if (connType.isEmpty())
            continue;

        m_connections[connType].clear();

        for (const auto &connObject : connList) {
            const QJsonObject &connection = connObject.toObject();

            m_connections[connType].append(connection);

            const auto &hwAddr = connection.value("HwAddress").toString();
            if (hwAddr.isEmpty()) {
                commonConnections[connType].append(connection);
            } else {
                deviceConnections[hwAddr][connType].append(connection);
            }

            const auto &interface = connection.value("IfcName").toString();
            if (interface.isEmpty()) {
                wiredCommonConnections[connType].append(connection);
            } else {
                wiredDeviceConnections[interface][connType].append(connection);
            }
        }
    }

    // ??? connections ????????????????????????
    for (NetworkDevice *dev : m_devices) {
        const QString &hwAddr = dev->realHwAdr();
        const QMap<QString, QList<QJsonObject>> &connsByType = deviceConnections.value(hwAddr);
        const QString &inter = dev->interfaceName();
        const QMap<QString, QList<QJsonObject>> &connsByInter = wiredDeviceConnections.value(inter);
        QList<QJsonObject> destConns;

        switch (dev->type()) {
        case NetworkDevice::Wired: {
            destConns += wiredCommonConnections.value("wired");
            destConns += connsByInter.value("wired");
            WiredDevice *wdDevice = static_cast<WiredDevice *>(dev);
            wdDevice->setConnections(destConns);
            break;
        }
        case NetworkDevice::Wireless: {
            destConns += commonConnections.value("wireless");
            destConns += connsByType.value("wireless");
            WirelessDevice *wsDevice = static_cast<WirelessDevice *>(dev);
            wsDevice->setConnections(destConns);

            destConns.clear();
            destConns += commonConnections.value("wireless-hotspot");
            destConns += connsByType.value("wireless-hotspot");
            wsDevice->setHotspotConnections(destConns);
            break;
        }
        default:
            break;
        }
    }

    Q_EMIT connectionListChanged();
}

void NetworkModel::onActiveConnInfoChanged(const QString &conns)
{
    m_activeConnInfos.clear();

    QMap<QString, QJsonObject> activeConnInfo;
    QMap<QString, QJsonObject> activeHotspotInfo;

    // parse active connections info and save it by DevicePath
    QJsonArray activeConns = QJsonDocument::fromJson(conns.toUtf8()).array();
    for (const auto &info : activeConns)
    {
        const auto &connInfo = info.toObject();
        const auto &type = connInfo.value("ConnectionType").toString();
        const auto &devPath = connInfo.value("Device").toString();

        activeConnInfo.insertMulti(devPath, connInfo);
        m_activeConnInfos << connInfo;

        if (type == "wireless-hotspot") {
            activeHotspotInfo.insert(devPath, connInfo);
        }
    }

    // update device active connection
    for (auto *dev : m_devices)
    {
        const auto &devPath = dev->path();

        switch (dev->type())
        {
        case NetworkDevice::Wired:
        {
            WiredDevice *d = static_cast<WiredDevice *>(dev);
            d->setActiveConnectionsInfo(activeConnInfo.values(devPath));
            break;
        }
        case NetworkDevice::Wireless:
        {
            WirelessDevice *d = static_cast<WirelessDevice *>(dev);
            d->setActiveConnectionsInfo(activeConnInfo.values(devPath));
            d->setActiveHotspotInfo(activeHotspotInfo.value(devPath));
            break;
        }
        default:;
        }
    }

    Q_EMIT activeConnInfoChanged(m_activeConnInfos);
}

void NetworkModel::onActiveConnectionsChanged(const QString &conns)
{
    m_activeConns.clear();

    // ???????????????????????? active ??????
    QMap<QString, QList<QJsonObject>> deviceActiveConnsMap;

    const QJsonObject activeConns = QJsonDocument::fromJson(conns.toUtf8()).object();
    for (auto it(activeConns.constBegin()); it != activeConns.constEnd(); ++it)
    {
        const QJsonObject &info = it.value().toObject();
        if (info.isEmpty())
            continue;

        m_activeConns << info;
        int connectionState = info.value("State").toInt();

        for (const auto &item : info.value("Devices").toArray()) {
            const QString &devicePath = item.toString();
            if (devicePath.isEmpty()) {
                continue;
            }
            deviceActiveConnsMap[devicePath] << info;

            NetworkDevice *dev = device(devicePath);
            if (dev != nullptr) {
                if (dev->status() != NetworkDevice::DeviceStatus::Activated && connectionState == CONNECTED) {
                    qDebug() << devicePath << "The active connection status does not match the device connection status. It has been changed";
                    dev->setDeviceStatus(NetworkDevice::DeviceStatus::Activated);
                }
            }
        }
    }

    // ??? active ??????????????????????????????
    for (auto it(deviceActiveConnsMap.constBegin()); it != deviceActiveConnsMap.constEnd(); ++it) {
        NetworkDevice *dev = device(it.key());
        if (dev == nullptr) {
            continue;
        }
        switch (dev->type()) {
            case NetworkDevice::Wired: {
                WiredDevice *wdDevice = static_cast<WiredDevice *>(dev);
                wdDevice->setActiveConnections(it.value());
                break;
            }
            case NetworkDevice::Wireless: {
                WirelessDevice *wsDevice = static_cast<WirelessDevice *>(dev);
                wsDevice->setActiveConnections(it.value());
                break;
            }
            default:
                break;
        }
    }

    Q_EMIT activeConnectionsChanged(m_activeConns);
}

void NetworkModel::onConnectionSessionCreated(const QString &device, const QString &sessionPath)
{
    for (const auto dev : m_devices)
    {
        if (dev->path() != device)
            continue;
        Q_EMIT dev->sessionCreated(sessionPath);
        return;
    }

    Q_EMIT unhandledConnectionSessionCreated(device, sessionPath);
}

void NetworkModel::onDeviceEnableChanged(const QString &device, const bool enabled)
{
    NetworkDevice *dev = nullptr;
    for (auto const d : m_devices)
    {
        if (d->path() == device)
        {
            dev = d;
            break;
        }
    }

    if (!dev)
        return;

    dev->setEnabled(enabled);

    Q_EMIT deviceEnableChanged(device, enabled);
}

void NetworkModel::onChainsTypeChanged(const QString &type)
{
    if (type != m_chainsProxy.type) {
        m_chainsProxy.type = type;
        Q_EMIT chainsTypeChanged(type);
    }
}

void NetworkModel::onChainsAddrChanged(const QString &addr)
{
    if (addr != m_chainsProxy.url) {
        m_chainsProxy.url = addr;
        Q_EMIT chainsAddrChanged(addr);
    }
}

void NetworkModel::onChainsPortChanged(const uint port)
{
    if (port != m_chainsProxy.port) {
        m_chainsProxy.port = port;
        Q_EMIT chainsPortChanged(port);
    }
}

void NetworkModel::onChainsUserChanged(const QString &user)
{
    if (user != m_chainsProxy.username) {
        m_chainsProxy.username = user;
        Q_EMIT chainsUsernameChanged(user);
    }
}

void NetworkModel::onChainsPasswdChanged(const QString &passwd)
{
    if (passwd != m_chainsProxy.password) {
        m_chainsProxy.password = passwd;
        Q_EMIT chainsPasswdChanged(passwd);
    }
}
void NetworkModel::onNeedSecrets(const QString &info)
{
    /*
     * TODO: there is a bug in daemon about var 'info', the value of key "DevicePath" is wrong.
     * here should be EMIT the needSecrets signal of specific device after the bug above fixed
    */

    //const QJsonObject &infoObject = QJsonDocument::fromJson(info.toUtf8()).object();
    //m_lastSecretDevice = device(infoObject.value("DevicePath").toString());

    /* TODO: check this signal(NeedSecrets) of daemon is only for wireless device */
    //if (m_lastSecretDevice != nullptr && m_lastSecretDevice->type() == NetworkDevice::Wireless) {
        //Q_EMIT static_cast<WirelessDevice *>(m_lastSecretDevice)->needSecrets(info);
    //} else {
        //m_lastSecretDevice = nullptr;
    //}

    Q_EMIT needSecrets(info);
}

void NetworkModel::onNeedSecretsFinished(const QString &info0, const QString &info1)
{
    /* TODO: same as above */

    //if (m_lastSecretDevice != nullptr) {
        //Q_EMIT static_cast<WirelessDevice *>(m_lastSecretDevice)->needSecretsFinished(info0, info1);
    //}

    Q_EMIT needSecretsFinished(info0, info1);
}

void NetworkModel::onConnectivityChanged(int connectivity)
{
    Connectivity conn = static_cast<Connectivity>(connectivity);
    if (m_Connectivity == conn) {
        return;
    }

    m_Connectivity = conn;

    // if the new connectivity state from NetworkManager is not Full,
    // check it again use our urls
    if (m_Connectivity != Full) {
        if (!m_connectivityCheckThread->isRunning()) {
            m_connectivityCheckThread->start();
        }
        Q_EMIT needCheckConnectivitySecondary();
    }

    Q_EMIT connectivityChanged(m_Connectivity);
}

void NetworkModel::onConnectivitySecondaryCheckFinished(bool connectivity)
{
    m_Connectivity = connectivity ? Full : NoConnectivity;
    Q_EMIT connectivityChanged(m_Connectivity);
}

bool NetworkModel::containsDevice(const QString &devPath) const
{
    return device(devPath) != nullptr;
}

NetworkDevice *NetworkModel::device(const QString &devPath) const
{
    for (auto const d : m_devices)
        if (d->path() == devPath)
            return d;

    return nullptr;
}

void NetworkModel::onAppProxyExistChanged(bool appProxyExist)
{
    if (m_appProxyExist == appProxyExist) {
        return;
    }

    m_appProxyExist = appProxyExist;

    Q_EMIT appProxyExistChanged(appProxyExist);
}

void NetworkModel::onWirelessAccessPointsChanged(const QString &WirelessList)
{
    //????????????json?????????,????????????????????????0,????????????for???????????????
    QJsonObject WirelessData = QJsonDocument::fromJson(WirelessList.toUtf8()).object();
    for (QString Device : WirelessData.keys()) {
        for (auto const dev : m_devices) {
            //????????????????????????,path?????????????????????device????????????????????????
            if (dev->type() != NetworkDevice::Wireless || dev->path() != Device) continue;
            dynamic_cast<WirelessDevice *>(dev)->setAPList(WirelessData.value(Device));
        }
    }
    return;
}
