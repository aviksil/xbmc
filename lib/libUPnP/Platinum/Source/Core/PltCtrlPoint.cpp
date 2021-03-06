/*****************************************************************
|
|   Platinum - Control Point
|
| Copyright (c) 2004-2008, Plutinosoft, LLC.
| All rights reserved.
| http://www.plutinosoft.com
|
| This program is free software; you can redistribute it and/or
| modify it under the terms of the GNU General Public License
| as published by the Free Software Foundation; either version 2
| of the License, or (at your option) any later version.
|
| OEMs, ISVs, VARs and other distributors that combine and 
| distribute commercially licensed software with Platinum software
| and do not wish to distribute the source code for the commercially
| licensed software under version 2, or (at your option) any later
| version, of the GNU General Public License (the "GPL") must enter
| into a commercial license agreement with Plutinosoft, LLC.
| 
| This program is distributed in the hope that it will be useful,
| but WITHOUT ANY WARRANTY; without even the implied warranty of
| MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
| GNU General Public License for more details.
|
| You should have received a copy of the GNU General Public License
| along with this program; see the file LICENSE.txt. If not, write to
| the Free Software Foundation, Inc., 
| 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
| http://www.gnu.org/licenses/gpl-2.0.html
|
****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "PltCtrlPoint.h"
#include "PltUPnP.h"
#include "PltDeviceData.h"
#include "PltXmlHelper.h"
#include "PltCtrlPointTask.h"
#include "PltSsdp.h"
#include "PltHttpServer.h"

NPT_SET_LOCAL_LOGGER("platinum.core.ctrlpoint")

/*----------------------------------------------------------------------
|   typedef
+---------------------------------------------------------------------*/
typedef PLT_HttpRequestHandler<PLT_CtrlPoint> PLT_HttpCtrlPointRequestHandler;

/*----------------------------------------------------------------------
|   PLT_CtrlPointListenerOnDeviceAddedIterator class
+---------------------------------------------------------------------*/
class PLT_CtrlPointListenerOnDeviceAddedIterator
{
public:
    PLT_CtrlPointListenerOnDeviceAddedIterator(PLT_DeviceDataReference& device) :
        m_Device(device) {}

    NPT_Result operator()(PLT_CtrlPointListener*& listener) const {
        return listener->OnDeviceAdded(m_Device);
    }

private:
    PLT_DeviceDataReference& m_Device;
};

/*----------------------------------------------------------------------
|   PLT_CtrlPointListenerOnDeviceRemovedIterator class
+---------------------------------------------------------------------*/
class PLT_CtrlPointListenerOnDeviceRemovedIterator
{
public:
    PLT_CtrlPointListenerOnDeviceRemovedIterator(PLT_DeviceDataReference& device) :
        m_Device(device) {}

    NPT_Result operator()(PLT_CtrlPointListener*& listener) const {
        return listener->OnDeviceRemoved(m_Device);
    }

private:
    PLT_DeviceDataReference& m_Device;
};

/*----------------------------------------------------------------------
|   PLT_CtrlPointListenerOnActionResponseIterator class
+---------------------------------------------------------------------*/
class PLT_CtrlPointListenerOnActionResponseIterator
{
public:
    PLT_CtrlPointListenerOnActionResponseIterator(NPT_Result           res, 
                                                  PLT_ActionReference& action, 
                                                  void*                userdata) :
        m_Res(res), m_Action(action), m_Userdata(userdata) {}

    NPT_Result operator()(PLT_CtrlPointListener*& listener) const {
        return listener->OnActionResponse(m_Res, m_Action, m_Userdata);
    }

private:
    NPT_Result           m_Res;
    PLT_ActionReference& m_Action;
    void*                m_Userdata;
};

/*----------------------------------------------------------------------
|   PLT_CtrlPointListenerOnEventNotifyIterator class
+---------------------------------------------------------------------*/
class PLT_CtrlPointListenerOnEventNotifyIterator
{
public:
    PLT_CtrlPointListenerOnEventNotifyIterator(PLT_Service*                  service, 
                                               NPT_List<PLT_StateVariable*>* vars) :
        m_Service(service), m_Vars(vars) {}

    NPT_Result operator()(PLT_CtrlPointListener*& listener) const {
        return listener->OnEventNotify(m_Service, m_Vars);
    }

private:
    PLT_Service*                  m_Service;
    NPT_List<PLT_StateVariable*>* m_Vars;
};

/*----------------------------------------------------------------------
|   PLT_AddGetSCPDRequestIterator class
+---------------------------------------------------------------------*/
class PLT_AddGetSCPDRequestIterator
{
public:
    PLT_AddGetSCPDRequestIterator(PLT_HttpClientSocketTask& task) :
        m_Task(task) {}

    NPT_Result operator()(PLT_Service*& service) const {
        // look for the host and port of the device
        NPT_String scpd_url = service->GetSCPDURL(true);

        NPT_LOG_INFO_3("Queueing SCPD request for service \"%s\" of device \"%s\" @ %s", 
            (const char*)service->GetServiceID(),
            (const char*)service->GetDevice()->GetFriendlyName(),
            (const char*)scpd_url);

        // Create request and attach service to it
        PLT_CtrlPointGetSCPDRequest* request = 
            new PLT_CtrlPointGetSCPDRequest(scpd_url, "GET", NPT_HTTP_PROTOCOL_1_1);
        request->m_Service = service;
        return m_Task.AddRequest((NPT_HttpRequest*)request);
    }

private:
    PLT_HttpClientSocketTask& m_Task;
};

/*----------------------------------------------------------------------
|   PLT_EventSubscriberRemoverIterator class
+---------------------------------------------------------------------*/
class PLT_EventSubscriberRemoverIterator
{
public:
    PLT_EventSubscriberRemoverIterator(PLT_CtrlPoint* ctrl_point) : 
        m_CtrlPoint(ctrl_point) { 
        m_CtrlPoint->m_Subscribers.Lock();
    }
    ~PLT_EventSubscriberRemoverIterator() {
        m_CtrlPoint->m_Subscribers.Unlock();
    }

    NPT_Result operator()(PLT_Service*& service) const {
        PLT_EventSubscriber* sub = NULL;
        if (NPT_SUCCEEDED(NPT_ContainerFind(m_CtrlPoint->m_Subscribers, 
                                            PLT_EventSubscriberFinderByService(service), sub))) {
            NPT_LOG_INFO_1("Removed subscriber \"%s\"", (const char*)sub->GetSID());
            m_CtrlPoint->m_Subscribers.Remove(sub);
            delete sub;
        }

        return NPT_SUCCESS;
    }

private:
    PLT_CtrlPoint* m_CtrlPoint;
};

/*----------------------------------------------------------------------
|   PLT_ServiceReadyIterator class
+---------------------------------------------------------------------*/
class PLT_ServiceReadyIterator
{
public:
    PLT_ServiceReadyIterator() {}

    NPT_Result operator()(PLT_Service*& service) const {
        return service->IsInitted()?NPT_SUCCESS:NPT_FAILURE;
    }
};

/*----------------------------------------------------------------------
|   PLT_DeviceReadyIterator class
+---------------------------------------------------------------------*/
class PLT_DeviceReadyIterator
{
public:
    PLT_DeviceReadyIterator() {}
    NPT_Result operator()(PLT_DeviceDataReference& device) const {
        NPT_Result res = device->m_Services.ApplyUntil(
            PLT_ServiceReadyIterator(), 
            NPT_UntilResultNotEquals(NPT_SUCCESS));
        if (NPT_FAILED(res)) return res;

        res = device->m_EmbeddedDevices.ApplyUntil(
            PLT_DeviceReadyIterator(), 
            NPT_UntilResultNotEquals(NPT_SUCCESS));
        if (NPT_FAILED(res)) return res;

        // a device must have at least one service or embedded device 
        // otherwise it's not ready
        if (device->m_Services.GetItemCount() == 0 &&
            device->m_EmbeddedDevices.GetItemCount() == 0) {
            return NPT_FAILURE;
        }
        
        return NPT_SUCCESS;
    }
};

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::PLT_CtrlPoint
+---------------------------------------------------------------------*/
PLT_CtrlPoint::PLT_CtrlPoint(const char* search_criteria /* = "upnp:rootdevice" */) :
    m_EventHttpServer(new PLT_HttpServer()),
    m_SearchCriteria(search_criteria)
{
    m_EventHttpServerHandler = new PLT_HttpCtrlPointRequestHandler(this);
    m_EventHttpServer->AddRequestHandler(m_EventHttpServerHandler, "/", true);
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::~PLT_CtrlPoint
+---------------------------------------------------------------------*/
PLT_CtrlPoint::~PLT_CtrlPoint()
{
    delete m_EventHttpServer;
    delete m_EventHttpServerHandler; 
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::IgnoreUUID
+---------------------------------------------------------------------*/
void
PLT_CtrlPoint::IgnoreUUID(const char* uuid)
{
    if (!m_UUIDsToIgnore.Find(NPT_StringFinder(uuid))) {
        m_UUIDsToIgnore.Add(uuid);
    }
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::Start
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::Start(PLT_SsdpListenTask* task)
{
    m_EventHttpServer->Start();

    // house keeping task
    m_TaskManager.StartTask(new PLT_CtrlPointHouseKeepingTask(this));

    task->AddListener(this);

    //    
    // use next line instead for DLNA testing, faster frequency for M-SEARCH
    //return m_SearchCriteria.GetLength()?Search(NPT_HttpUrl("239.255.255.250", 1900, "*"), m_SearchCriteria, 1, 5000):NPT_SUCCESS;
    // 
    
    return m_SearchCriteria.GetLength()?Search(NPT_HttpUrl("239.255.255.250", 1900, "*"), m_SearchCriteria):NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::Stop
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::Stop(PLT_SsdpListenTask* task)
{
    task->RemoveListener(this);

    m_TaskManager.StopAllTasks();
    m_EventHttpServer->Stop();

    // we can safely clear everything without a lock
    // as there are no more tasks pending
    m_Devices.Clear();

    m_Subscribers.Apply(NPT_ObjectDeleter<PLT_EventSubscriber>());
    m_Subscribers.Clear();

    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::AddListener
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::AddListener(PLT_CtrlPointListener* listener) 
{
    NPT_AutoLock lock(m_ListenerList);
    if (!m_ListenerList.Contains(listener)) {
        m_ListenerList.Add(listener);
    }
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::RemoveListener
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::RemoveListener(PLT_CtrlPointListener* listener)
{
    NPT_AutoLock lock(m_ListenerList);
    m_ListenerList.Remove(listener);
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::CreateSearchTask
+---------------------------------------------------------------------*/
PLT_SsdpSearchTask*
PLT_CtrlPoint::CreateSearchTask(const NPT_HttpUrl&   url, 
                                const char*          target, 
                                NPT_Cardinal         mx, 
                                NPT_Timeout          frequency,
                                const NPT_IpAddress& address)
{
    // make sure mx is at least 1
    if (mx<1) mx=1;

    // create socket
    NPT_UdpMulticastSocket* socket = new NPT_UdpMulticastSocket();
    socket->SetInterface(address);
    socket->SetTimeToLive(4);

    // bind to something > 1024 and different than 1900
    int retries = 20;
    do {    
        int random = NPT_System::GetRandomInteger();
        int port = (unsigned short)(1024 + (random % 15000));
        if (port == 1900) continue;

        if (NPT_SUCCEEDED(socket->Bind(
            NPT_SocketAddress(NPT_IpAddress::Any, port), 
            false)))
            break;

    } while (--retries);

    if (retries == 0) {
        NPT_LOG_SEVERE("Couldn't bind socket for Search Task");
        return NULL;
    }

    // create request
    NPT_HttpRequest* request = new NPT_HttpRequest(url, "M-SEARCH", NPT_HTTP_PROTOCOL_1_1);
    PLT_UPnPMessageHelper::SetMX(*request, mx);
    PLT_UPnPMessageHelper::SetST(*request, target);
    PLT_UPnPMessageHelper::SetMAN(*request, "\"ssdp:discover\"");
    request->GetHeaders().SetHeader(NPT_HTTP_HEADER_USER_AGENT, NPT_HttpClient::m_UserAgentHeader);

    // create task
    PLT_SsdpSearchTask* task = new PLT_SsdpSearchTask(
        socket,
        this, 
        request,
        frequency<(NPT_Timeout)mx*5000?(NPT_Timeout)mx*5000:frequency);
    return task;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::Search
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::Search(const NPT_HttpUrl& url, 
                      const char*        target, 
                      NPT_Cardinal       mx /* = 5 */,
                      NPT_Timeout        frequency /* = 50000 */)
{
    NPT_List<NPT_NetworkInterface*> if_list;
    NPT_List<NPT_NetworkInterface*>::Iterator net_if;
    NPT_List<NPT_NetworkInterfaceAddress>::Iterator net_if_addr;

    NPT_CHECK_SEVERE(PLT_UPnPMessageHelper::GetNetworkInterfaces(if_list, true));

    for (net_if = if_list.GetFirstItem(); 
         net_if; 
         net_if++) {
        // make sure the interface is at least broadcast or multicast
        if (!((*net_if)->GetFlags() & NPT_NETWORK_INTERFACE_FLAG_MULTICAST) &&
            !((*net_if)->GetFlags() & NPT_NETWORK_INTERFACE_FLAG_BROADCAST)) {
            continue;
        }       
            
        for (net_if_addr = (*net_if)->GetAddresses().GetFirstItem(); 
             net_if_addr; 
             net_if_addr++) {
            // create task
            PLT_SsdpSearchTask* task = CreateSearchTask(url, 
                target, 
                mx, 
                frequency,
                (*net_if_addr).GetPrimaryAddress());
            m_TaskManager.StartTask(task);
        }
    }

    if_list.Apply(NPT_ObjectDeleter<NPT_NetworkInterface>());
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::Discover
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::Discover(const NPT_HttpUrl& url, 
                        const char*        target, 
                        NPT_Cardinal       mx /* = 5 */,
                        NPT_Timeout        frequency /* = 50000 */)
{
    // make sure mx is at least 1
    if (mx<1) mx = 1;

    // create socket
    NPT_UdpSocket* socket = new NPT_UdpSocket();

    // create request
    NPT_HttpRequest* request = new NPT_HttpRequest(url, "M-SEARCH", NPT_HTTP_PROTOCOL_1_1);
    PLT_UPnPMessageHelper::SetMX(*request, mx);
    PLT_UPnPMessageHelper::SetST(*request, target);
    PLT_UPnPMessageHelper::SetMAN(*request, "\"ssdp:discover\"");
    request->GetHeaders().SetHeader(NPT_HTTP_HEADER_USER_AGENT, NPT_HttpClient::m_UserAgentHeader);

    // force HOST to be the regular multicast address:port
    // Some servers do care (like WMC) otherwise they won't respond to us
    request->GetHeaders().SetHeader(NPT_HTTP_HEADER_HOST, "239.255.255.250:1900");

    // create task
    PLT_ThreadTask* task = new PLT_SsdpSearchTask(
        socket,
        this, 
        request,
        frequency<(NPT_Timeout)mx*5000?(NPT_Timeout)mx*5000:frequency);  /* repeat no less than every 5 secs */
    return m_TaskManager.StartTask(task);
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::DoHouseKeeping
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::DoHouseKeeping()
{
    NPT_List<PLT_DeviceDataReference> devices_to_remove;
    
    // remove expired devices
    {
        NPT_AutoLock lock(m_Devices);

        PLT_DeviceDataReference head, device;
        while (NPT_SUCCEEDED(m_Devices.PopHead(device))) {
            NPT_TimeStamp    last_update = device->GetLeaseTimeLastUpdate();
            NPT_TimeInterval lease_time  = device->GetLeaseTime();

            // check if device lease time has expired or if failed to renew subscribers 
            NPT_TimeStamp now;
            NPT_System::GetCurrentTimeStamp(now);
            if (now > last_update + NPT_TimeInterval((unsigned long)(((float)lease_time)*2), 0)) {
                devices_to_remove.Add(device);
            } else {
                // add the device back to our list since it is still alive
                m_Devices.Add(device);

                // keep track of first device added back to list
                // to know when checked all devices in initial list
                if (head.IsNull()) head = device;
            }
            
            // have we exhausted initial list?
            if (!head.IsNull() && head == *m_Devices.GetFirstItem())
                break;
        };
    }

    // remove old devices
    {
        for (NPT_List<PLT_DeviceDataReference>::Iterator device = 
             devices_to_remove.GetFirstItem();
             device;
             device++) {
             RemoveDevice(*device);
        }
    }

    // renew subscribers of subscribed device services
    {
        NPT_AutoLock lock(m_Subscribers);
        NPT_List<PLT_EventSubscriber*>::Iterator sub = 
            m_Subscribers.GetFirstItem();
        while (sub) {
            NPT_TimeStamp now;
            NPT_System::GetCurrentTimeStamp(now);

            // time to renew if within 5 secs of expiration
            if (now > (*sub)->GetExpirationTime() - NPT_TimeStamp(5, 0)) {
                RenewSubscriber(*(*sub));
            }
            sub++;
        }
    }
    
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::FindDevice
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::FindDevice(const char*              uuid, 
                          PLT_DeviceDataReference& device,
                          bool                     return_root /* = false */) 
{
    for (NPT_List<PLT_DeviceDataReference>::Iterator iter =
            m_Devices.GetFirstItem();
         iter;
         iter++) {
         if ((*iter)->GetUUID().Compare(uuid) == 0) {
            device = *iter;
            return NPT_SUCCESS;
         }
         if (NPT_SUCCEEDED((*iter)->FindEmbeddedDevice(uuid, device))) {
             // return root instead if specified
             if (return_root) device = (*iter);
             return NPT_SUCCESS;
         }
    }

    return NPT_ERROR_NO_SUCH_ITEM;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::FindActionDesc
+---------------------------------------------------------------------*/
NPT_Result 
PLT_CtrlPoint::FindActionDesc(PLT_DeviceDataReference& device, 
                              const char*              service_type,
                              const char*              action_name,
                              PLT_ActionDesc*&         action_desc)
{
    // look for the service
    PLT_Service* service;
    if (NPT_FAILED(device->FindServiceByType(service_type, service))) {
        NPT_LOG_FINE_1("Service %s not found", (const char*)service_type);
        return NPT_FAILURE;
    }

    action_desc = service->FindActionDesc(action_name);
    if (action_desc == NULL) {
        NPT_LOG_FINE_1("Action %s not found in service", action_name);
        return NPT_FAILURE;
    }

    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::CreateAction
+---------------------------------------------------------------------*/
NPT_Result 
PLT_CtrlPoint::CreateAction(PLT_DeviceDataReference& device, 
                            const char*              service_type,
                            const char*              action_name,
                            PLT_ActionReference&     action)
{
    PLT_ActionDesc* action_desc;
    NPT_CHECK_SEVERE(FindActionDesc(device, 
        service_type, 
        action_name, 
        action_desc));

    PLT_DeviceDataReference root_device;
    {
        NPT_AutoLock lock_devices(m_Devices);
        NPT_CHECK_SEVERE(FindDevice(device->GetUUID(), root_device, true));
    }

    action = new PLT_Action(*action_desc, root_device);
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessHttpRequest
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessHttpRequest(NPT_HttpRequest&              request,
                                  const NPT_HttpRequestContext& context,
                                  NPT_HttpResponse&             response)
{
    NPT_COMPILER_UNUSED(context);
    if (!request.GetMethod().Compare("NOTIFY")) {
        return ProcessHttpNotify(request, context, response);
    }

    NPT_LOG_SEVERE("CtrlPoint received bad http request\r\n");
    response.SetStatus(412, "Precondition Failed");
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessHttpNotify
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessHttpNotify(NPT_HttpRequest&              request,
                                 const NPT_HttpRequestContext& context,
                                 NPT_HttpResponse&             response)
{
    NPT_COMPILER_UNUSED(context);

    NPT_List<PLT_StateVariable*> vars;
    PLT_EventSubscriber*         sub = NULL;    
    NPT_String                   str;
    NPT_XmlElementNode*          xml = NULL;
    NPT_String                   callback_uri;
    NPT_String                   uuid;
    NPT_String                   service_id;
    NPT_UInt32                   seq = 0;
    PLT_Service*                 service = NULL;
    PLT_DeviceData*              device = NULL;
    NPT_String                   content_type;

    NPT_String method   = request.GetMethod();
    NPT_String uri      = request.GetUrl().GetPath();

    PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINER, &request);

    const NPT_String* sid = PLT_UPnPMessageHelper::GetSID(request);
    const NPT_String* nt  = PLT_UPnPMessageHelper::GetNT(request);
    const NPT_String* nts = PLT_UPnPMessageHelper::GetNTS(request);
    PLT_HttpHelper::GetContentType(request, content_type);

    if (!sid || sid->GetLength() == 0) { 
        NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
    }
    
    if (!nt  || nt->GetLength()  == 0 || 
        !nts || nts->GetLength() == 0) {
        response.SetStatus(400, "Bad request");
        NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
    }
    
    {
        NPT_AutoLock lock_subs(m_Subscribers);

        // look for the subscriber with that subscription url
        if (NPT_FAILED(NPT_ContainerFind(m_Subscribers, 
                                         PLT_EventSubscriberFinderBySID(*sid), 
                                         sub))) {
            NPT_LOG_FINE_1("Subscriber %s not found\n", (const char*)*sid);
            NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
        }

        // verify the request is syntactically correct
        service = sub->GetService();
        device  = service->GetDevice();

        uuid = device->GetUUID();
        service_id = service->GetServiceID();

        // callback uri for this sub
        callback_uri = "/" + uuid + "/" + service_id;

        if (uri.Compare(callback_uri, true) ||
            nt->Compare("upnp:event", true) || 
            nts->Compare("upnp:propchange", true)) {
            NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
        }

        // if the sequence number is less than our current one, we got it out of order
        // so we disregard it
        PLT_UPnPMessageHelper::GetSeq(request, seq);
        if (sub->GetEventKey() && seq < sub->GetEventKey()) {
            NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
        }

        // parse body
        if (NPT_FAILED(PLT_HttpHelper::ParseBody(request, xml))) {
            NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
        }

        // check envelope
        if (xml->GetTag().Compare("propertyset", true)) {
            NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
        }

        // check property set
        // keep a vector of the state variables that changed
        NPT_XmlElementNode* property;
        PLT_StateVariable*  var;
        for (NPT_List<NPT_XmlNode*>::Iterator children = xml->GetChildren().GetFirstItem(); 
             children; 
             children++) {
            NPT_XmlElementNode* child = (*children)->AsElementNode();
            if (!child) continue;

            // check property
            if (child->GetTag().Compare("property", true)) continue;

            if (NPT_FAILED(PLT_XmlHelper::GetChild(child, property))) {
                NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
            }

            var = service->FindStateVariable(property->GetTag());
            if (var == NULL) continue;

            if (NPT_FAILED(var->SetValue(property->GetText()?*property->GetText():""))) {
                NPT_CHECK_LABEL_WARNING(NPT_FAILURE, bad_request);
            }
            vars.Add(var);
        }    

        // update sequence
        sub->SetEventKey(seq);
    }

    // notify listener we got an update
    if (vars.GetItemCount()) {
        NPT_AutoLock lock(m_ListenerList);
        m_ListenerList.Apply(PLT_CtrlPointListenerOnEventNotifyIterator(service, &vars));
    }

    delete xml;
    return NPT_SUCCESS;

bad_request:
    NPT_LOG_SEVERE("CtrlPoint received bad request\r\n");
    if (response.GetStatusCode() == 200) {
        response.SetStatus(412, "Precondition Failed");
    }
    delete xml;
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessSsdpSearchResponse
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessSsdpSearchResponse(NPT_Result                    res, 
                                         const NPT_HttpRequestContext& context, 
                                         NPT_HttpResponse*             response)
{
    NPT_CHECK_SEVERE(res);
    NPT_CHECK_POINTER_SEVERE(response);

    NPT_String ip_address = context.GetRemoteAddress().GetIpAddress().ToString();
    NPT_String protocol   = response->GetProtocol();
    
    NPT_LOG_FINE_2("CtrlPoint received SSDP search response from %s:%d",
        (const char*)context.GetRemoteAddress().GetIpAddress().ToString() , 
        context.GetRemoteAddress().GetPort());
    PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINER, response);
    
    // any 2xx responses are ok
    if (response->GetStatusCode()/100 == 2) {
        const NPT_String* st  = response->GetHeaders().GetHeaderValue("st");
        const NPT_String* usn = response->GetHeaders().GetHeaderValue("usn");
        const NPT_String* ext = response->GetHeaders().GetHeaderValue("ext");
        NPT_CHECK_POINTER_SEVERE(st);
        NPT_CHECK_POINTER_SEVERE(usn);
        NPT_CHECK_POINTER_SEVERE(ext);
        
        NPT_String uuid;
        // if we get an advertisement other than uuid
        // verify it's formatted properly
        if (usn != st) {
            char tmp_uuid[200];
            char tmp_st[200];
            int  ret;
            // FIXME: We can't use sscanf directly!
            ret = sscanf(((const char*)*usn)+5, "%199[^::]::%199s",
                tmp_uuid, 
                tmp_st);
            if (ret != 2)
                return NPT_FAILURE;
            
            if (st->Compare(tmp_st, true))
                return NPT_FAILURE;
            
            uuid = tmp_uuid;
        } else {
            uuid = ((const char*)*usn)+5;
        }
        
        if (m_UUIDsToIgnore.Find(NPT_StringFinder(uuid))) {
            NPT_LOG_FINE_1("CtrlPoint received a search response from ourselves (%s)\n", (const char*)uuid);
            return NPT_SUCCESS;
        }

        return ProcessSsdpMessage(response, context, uuid);    
    }
    
    return NPT_FAILURE;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::OnSsdpPacket
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::OnSsdpPacket(NPT_HttpRequest&              request,
                            const NPT_HttpRequestContext& context)
{
    return ProcessSsdpNotify(request, context);
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessSsdpNotify
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessSsdpNotify(NPT_HttpRequest&              request, 
                                 const NPT_HttpRequestContext& context)
{
    // get the address of who sent us some data back
    NPT_String ip_address = context.GetRemoteAddress().GetIpAddress().ToString();
    NPT_String method     = request.GetMethod();
    NPT_String uri        = (const char*)request.GetUrl().GetPath();
    NPT_String protocol   = request.GetProtocol();

    if (method.Compare("NOTIFY") == 0) {
        NPT_LOG_INFO_2("Received SSDP NOTIFY from %s:%d",
            context.GetRemoteAddress().GetIpAddress().ToString().GetChars(), 
            context.GetRemoteAddress().GetPort());
        PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINER, &request);

        if ((uri.Compare("*") != 0) || (protocol.Compare("HTTP/1.1") != 0))
            return NPT_FAILURE;
        
        const NPT_String* nts = PLT_UPnPMessageHelper::GetNTS(request);
        const NPT_String* nt  = PLT_UPnPMessageHelper::GetNT(request);
        const NPT_String* usn = PLT_UPnPMessageHelper::GetUSN(request);
        NPT_CHECK_POINTER_SEVERE(nts);
        NPT_CHECK_POINTER_SEVERE(nt);
        NPT_CHECK_POINTER_SEVERE(usn);

        NPT_String uuid;
        // if we get an advertisement other than uuid
        // verify it's formatted properly
        if (*usn != *nt) {
            char tmp_uuid[200];
            char tmp_nt[200];
            int  ret;
            //FIXME: no sscanf!
            ret = sscanf(((const char*)*usn)+5, "%199[^::]::%199s",
                tmp_uuid, 
                tmp_nt);
            if (ret != 2)
                return NPT_FAILURE;
            
            if (nt->Compare(tmp_nt, true))
                return NPT_FAILURE;
            
            uuid = tmp_uuid;
        } else {
            uuid = ((const char*)*usn)+5;
        }

        if (m_UUIDsToIgnore.Find(NPT_StringFinder(uuid))) {
            NPT_LOG_FINE_1("Received a NOTIFY request from ourselves (%s)\n", (const char*)uuid);
            return NPT_SUCCESS;
        }

        // if it's a byebye, remove the device and return right away
        if (nts->Compare("ssdp:byebye", true) == 0) {
            NPT_LOG_INFO_1("Received a byebye NOTIFY request from %s\n", (const char*)uuid);
            
            PLT_DeviceDataReference root_device;
            
            {
                // look for root device
                NPT_AutoLock lock_devices(m_Devices);
                FindDevice(uuid, root_device, true);
            }
                
            if (!root_device.IsNull()) RemoveDevice(root_device);
            return NPT_SUCCESS;
        }
        
        return ProcessSsdpMessage(&request, context, uuid);
    }
    
    return NPT_FAILURE;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::AddDevice
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::AddDevice(PLT_DeviceDataReference& data)
{
    NPT_AutoLock lock(m_ListenerList);
    return NotifyDeviceReady(data);
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::NotifyDeviceReady
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::NotifyDeviceReady(PLT_DeviceDataReference& data)
{
    m_ListenerList.Apply(PLT_CtrlPointListenerOnDeviceAddedIterator(data));

    /* recursively add embedded devices */
    NPT_Array<PLT_DeviceDataReference> embedded_devices = 
        data->GetEmbeddedDevices();
    for(NPT_Cardinal i=0;i<embedded_devices.GetItemCount();i++) {
        NotifyDeviceReady(embedded_devices[i]);
    }
    
    return NPT_SUCCESS;
}


/*----------------------------------------------------------------------
|   PLT_CtrlPoint::RemoveDevice
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::RemoveDevice(PLT_DeviceDataReference& data)
{
    {
        NPT_AutoLock lock(m_ListenerList);
        NotifyDeviceRemoved(data);
    }
    
    {
        NPT_AutoLock lock(m_Devices);
        CleanupDevice(data);
    }
    
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::NotifyDeviceRemoved
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::NotifyDeviceRemoved(PLT_DeviceDataReference& data)
{
    m_ListenerList.Apply(PLT_CtrlPointListenerOnDeviceRemovedIterator(data));

    /* recursively add embedded devices */
    NPT_Array<PLT_DeviceDataReference> embedded_devices = 
        data->GetEmbeddedDevices();
    for(NPT_Cardinal i=0;i<embedded_devices.GetItemCount();i++) {
        NotifyDeviceRemoved(embedded_devices[i]);
    }
    
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::CleanupDevice
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::CleanupDevice(PLT_DeviceDataReference& data)
{
    NPT_LOG_INFO_1("Removing %s from device list\n", (const char*)data->GetUUID());
    
    /* recursively remove embedded devices */
    NPT_Array<PLT_DeviceDataReference> embedded_devices = 
        data->GetEmbeddedDevices();
    for(NPT_Cardinal i=0;i<embedded_devices.GetItemCount();i++) {
        CleanupDevice(embedded_devices[i]);
    }

    /* remove from list */
    m_Devices.Remove(data);

    /* unsubscribe from services */
    data->m_Services.Apply(PLT_EventSubscriberRemoverIterator(this));

    /* remove from parent */
    PLT_DeviceDataReference parent;
    if (!data->GetParentUUID().IsEmpty() &&
        NPT_SUCCEEDED(FindDevice(data->GetParentUUID(), parent))) {
        parent->RemoveEmbeddedDevice(data);
    }

    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessSsdpMessage
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessSsdpMessage(NPT_HttpMessage*              message, 
                                  const NPT_HttpRequestContext& context,
                                  NPT_String&                   uuid)
{
    NPT_COMPILER_UNUSED(context);
    NPT_CHECK_POINTER_SEVERE(message);

    if (m_UUIDsToIgnore.Find(NPT_StringFinder(uuid))) return NPT_SUCCESS;

    const NPT_String* location = PLT_UPnPMessageHelper::GetLocation(*message);
    NPT_CHECK_POINTER_SEVERE(location);
    
    // be nice and assume a default lease time if not found
    NPT_Timeout leasetime;
    if (NPT_FAILED(PLT_UPnPMessageHelper::GetLeaseTime(*message, leasetime))) {
        leasetime = (NPT_Timeout)PLT_Constants::GetInstance().m_DefaultSubscribeLease;
    }

    {
        NPT_AutoLock lock(m_Devices);
        PLT_DeviceDataReference data;
        if (NPT_SUCCEEDED(FindDevice(uuid, data))) {  
            /*
            // in case we missed the byebye and the device description has changed (ip or port)
            // reset base and assumes device is the same (same number of services and SCPDs)
            // FIXME: The right way is to remove the device and rescan it though
            PLT_DeviceReadyIterator device_tester;
            if (NPT_SUCCEEDED(device_tester(data)) && 
                data->GetDescriptionUrl().Compare(*location, true)) {
                NPT_LOG_INFO_2("Old device \"%s\" detected @ new location %s", 
                    (const char*)data->GetFriendlyName(), 
                    
                    location->GetChars());
                data->SetURLBase(NPT_HttpUrl(*location));
            } */

            // renew expiration time
            data->SetLeaseTime(NPT_TimeInterval(leasetime, 0));
            NPT_LOG_FINE_1("Device \"%s\" expiration time renewed..", 
                (const char*)data->GetFriendlyName());

            return NPT_SUCCESS;
        }

        // Inspect new device only if it's ssdp messge of a rootdevice
        const NPT_String* nt = PLT_UPnPMessageHelper::GetNT(*message);
        const NPT_String* st = PLT_UPnPMessageHelper::GetST(*message);
        if ((nt && !nt->Compare("upnp:rootdevice")) ||
            (st && !st->Compare("upnp:rootdevice"))) {
            return InspectDevice(*location, uuid, leasetime);
        }
    }
    
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::InspectDevice
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::InspectDevice(const char* location, 
                             const char* uuid, 
                             NPT_Timeout leasetime)
{
    NPT_HttpUrl url(location);
    if (!url.IsValid()) return NPT_FAILURE;

    NPT_LOG_INFO_2("New device \"%s\" detected @ %s", uuid, location);

    PLT_DeviceDataReference data(
        new PLT_DeviceData(url, uuid, NPT_TimeInterval(leasetime, 0)));
    m_Devices.Add(data);
        
    // Start a task to retrieve the description
    PLT_CtrlPointGetDescriptionTask* task = new PLT_CtrlPointGetDescriptionTask(
        url,
        this, 
        data);

    // Add a delay to make sure that we received all NOTIFY bye-bye
    // and processed them since they may be sent before during device
    // bootup but received later (udp)
    NPT_TimeInterval delay(1.0f);
    m_TaskManager.StartTask(task, &delay);

    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::FetchDeviceSCPDs
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::FetchDeviceSCPDs(PLT_HttpClientSocketTask& task,
                                PLT_DeviceDataReference&  device, 
                                NPT_Cardinal              level)
{
    if (level == 5 && device->m_EmbeddedDevices.GetItemCount()) {
        NPT_LOG_FATAL("Too many embedded devices depth! ");
        return NPT_FAILURE;
    }

    ++level;

    // add embedded devices to list of devices
    // and fetch their services scpd
    for (NPT_Cardinal i = 0;
         i<device->m_EmbeddedDevices.GetItemCount();
         i++) {

         NPT_CHECK(FetchDeviceSCPDs(task, device->m_EmbeddedDevices[i], level));
    }

    // Get SCPD of root device services now
    return device->m_Services.Apply(PLT_AddGetSCPDRequestIterator(task));
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessGetDescriptionResponse
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessGetDescriptionResponse(NPT_Result                    res, 
                                             const NPT_HttpRequestContext& context,
                                             NPT_HttpResponse*             response, 
                                             PLT_DeviceDataReference&      root_device)
{    
    PLT_CtrlPointGetSCPDTask* task = NULL;
    NPT_String desc;

    NPT_LOG_INFO_2("Received device description for %s (result = %d)", 
        (const char*)root_device->GetUUID(), 
        res);

    // verify response was ok
    NPT_CHECK_LABEL_FATAL(res, bad_response);
    NPT_CHECK_POINTER_LABEL_FATAL(response, bad_response);

    PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINER, response);

    // get response body
    res = PLT_HttpHelper::GetBody(*response, desc);
    NPT_CHECK_LABEL_SEVERE(res, bad_response);
    
    {
        NPT_AutoLock lock(m_Devices);

        // make sure root device hasn't disappeared
        PLT_DeviceDataReference device;
        NPT_CHECK_LABEL_WARNING(FindDevice(root_device->GetUUID(), device), 
                                bad_response);

        // set the device description
        res = root_device->SetDescription(desc, 
                                          context.GetLocalAddress().GetIpAddress());
        NPT_CHECK_LABEL_SEVERE(res, bad_response);

        NPT_LOG_INFO_2("Device \"%s\" is now known as \"%s\"", 
            (const char*)device->GetUUID(), 
            (const char*)device->GetFriendlyName());

        // create one single task to fetch all scpds one after the other
        task = new PLT_CtrlPointGetSCPDTask(this, 
                                            (PLT_DeviceDataReference&)root_device);
        NPT_CHECK_LABEL_SEVERE(FetchDeviceSCPDs(*task, root_device, 0), 
                               bad_response);

        // Add a delay, some devices need it (aka Rhapsody)
        NPT_TimeInterval delay(0.1f);

        // if device has embedded devices, we want to delay fetching scpds
        // just in case there's a chance all the initial NOTIFY bye-bye have
        // not all been received yet which would cause to remove the devices
        // as we're adding them
        if (root_device->m_EmbeddedDevices.GetItemCount() > 0) delay = 1.f;
        m_TaskManager.StartTask(task, &delay);
    }

    return NPT_SUCCESS;

bad_response:
    NPT_LOG_SEVERE_2("Bad Description response for device \"%s\": %s", 
        (const char*)root_device->GetUUID(),
        (const char*)desc);

    RemoveDevice(root_device);
    
    if (task) delete task;
    return res;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessGetSCPDResponse
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessGetSCPDResponse(NPT_Result                   res, 
                                      PLT_CtrlPointGetSCPDRequest* request,
                                      NPT_HttpResponse*            response,
                                      PLT_DeviceDataReference&     root_device)
{
    PLT_DeviceReadyIterator device_tester;
    NPT_String              scpd;

    NPT_LOG_INFO_3("Received SCPD response for a service of device \"%s\" @ %s (result = %d)", 
        (const char*)root_device->GetFriendlyName(), 
        (const char*)request->GetUrl().ToString(),
        res);

    // verify response was ok
    NPT_CHECK_LABEL_FATAL(res, bad_response);
    NPT_CHECK_POINTER_LABEL_FATAL(request, bad_response);
    NPT_CHECK_POINTER_LABEL_FATAL(response, bad_response);

    PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINER, response);

    // get response body
    res = PLT_HttpHelper::GetBody(*response, scpd);
    NPT_CHECK_LABEL_FATAL(res, bad_response);

    {
        NPT_AutoLock lock(m_Devices);

        // make sure root device hasn't disappeared
        PLT_DeviceDataReference device;
        NPT_CHECK_LABEL_WARNING(FindDevice(root_device->GetUUID(), device), 
                                bad_response);
    }
        
    // set the service scpd
    res = request->m_Service->SetSCPDXML(scpd);
    NPT_CHECK_LABEL_SEVERE(res, bad_response);

    // if root device is ready, notify listeners about it and embedded devices
    if (NPT_SUCCEEDED(device_tester(root_device))) {
        AddDevice(root_device);
    }
    
    return NPT_SUCCESS;

bad_response:
    NPT_LOG_SEVERE_2("Bad SCPD response for device \"%s\":%s", 
        (const char*)root_device->GetFriendlyName(),
        (const char*)scpd);

    RemoveDevice(root_device);
    return res;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::RenewSubscriber
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::RenewSubscriber(PLT_EventSubscriber& subscriber)
{
    // look for the corresponding root device
    PLT_DeviceDataReference root_device;
    {
        NPT_AutoLock lock_devices(m_Devices);
        NPT_CHECK_WARNING(FindDevice(
            subscriber.GetService()->GetDevice()->GetUUID(), 
            root_device,
            true));
    }

    NPT_LOG_FINE_3("Renewing subscriber \"%s\" for service \"%s\" of device \"%s\"", 
        (const char*)subscriber.GetSID(),
        (const char*)subscriber.GetService()->GetServiceID(),
        (const char*)subscriber.GetService()->GetDevice()->GetFriendlyName());

    // create the request
    NPT_HttpRequest* request = new NPT_HttpRequest(
        subscriber.GetService()->GetEventSubURL(true), 
        "SUBSCRIBE", 
        NPT_HTTP_PROTOCOL_1_1);

    PLT_UPnPMessageHelper::SetSID(*request, subscriber.GetSID());
    PLT_UPnPMessageHelper::SetTimeOut(*request, (NPT_Int32)PLT_Constants::GetInstance().m_DefaultSubscribeLease);

    // Prepare the request
    // create a task to post the request
    PLT_ThreadTask* task = new PLT_CtrlPointSubscribeEventTask(
        request,
        this, 
        root_device,
        subscriber.GetService());
    return m_TaskManager.StartTask(task);
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::Subscribe
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::Subscribe(PLT_Service* service, 
                         bool         cancel, 
                         void*        userdata)
{
    NPT_HttpRequest* request = NULL;

    // make sure service is subscribable
    if (!service->IsSubscribable()) return NPT_FAILURE;

    // event url
    NPT_HttpUrl url(service->GetEventSubURL(true));

    // look for the corresponding root device
    PLT_DeviceDataReference root_device;
    {
        NPT_AutoLock lock_devices(m_Devices);
        NPT_CHECK_WARNING(FindDevice(service->GetDevice()->GetUUID(), 
                                     root_device,
                                     true));
    }

    {
        // look for the subscriber with that service to decide if it's a renewal or not
        NPT_AutoLock lock(m_Subscribers);
        PLT_EventSubscriber* sub = NULL;
        NPT_ContainerFind(m_Subscribers, 
                          PLT_EventSubscriberFinderByService(service), 
                          sub);

        if (cancel == false) {
            // renewal?
            if (sub) return RenewSubscriber(*sub);

            NPT_LOG_INFO_2("Subscribing to service \"%s\" of device \"%s\"",
                (const char*)service->GetServiceID(),
                (const char*)service->GetDevice()->GetFriendlyName());

            // prepare the callback url
            NPT_String uuid         = service->GetDevice()->GetUUID();
            NPT_String service_id   = service->GetServiceID();
            NPT_String callback_uri = "/" + uuid + "/" + service_id;

            // create the request
            request = new NPT_HttpRequest(url, "SUBSCRIBE", NPT_HTTP_PROTOCOL_1_1);
            // specify callback url using ip of interface used when 
            // retrieving device description
            NPT_HttpUrl callbackUrl(
                service->GetDevice()->m_LocalIfaceIp.ToString(), 
                m_EventHttpServer->GetPort(), 
                callback_uri);

            // set the required headers for a new subscription
            PLT_UPnPMessageHelper::SetNT(*request, "upnp:event");
            PLT_UPnPMessageHelper::SetCallbacks(*request, 
                "<" + callbackUrl.ToString() + ">");
            PLT_UPnPMessageHelper::SetTimeOut(*request, (NPT_Int32)PLT_Constants::GetInstance().m_DefaultSubscribeLease);
        } else {
            NPT_LOG_INFO_3("Unsubscribing subscriber \"%s\" for service \"%s\" of device \"%s\"",
                (const char*)(sub?sub->GetSID().GetChars():"unknown"),
                (const char*)service->GetServiceID(),
                (const char*)service->GetDevice()->GetFriendlyName());        
            
            // cancellation
            if (!sub) return NPT_FAILURE;

            // create the request
            request = new NPT_HttpRequest(url, "UNSUBSCRIBE", NPT_HTTP_PROTOCOL_1_1);
            PLT_UPnPMessageHelper::SetSID(*request, sub->GetSID());

            // remove from list now
            m_Subscribers.Remove(sub, true);
            delete sub;
        }
    }

    // verify we have request to send just in case
    NPT_CHECK_POINTER_FATAL(request);

    // Prepare the request
    // create a task to post the request
    PLT_ThreadTask* task = new PLT_CtrlPointSubscribeEventTask(
        request,
        this, 
		root_device,
        service, 
        userdata);
    m_TaskManager.StartTask(task);

    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessSubscribeResponse
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessSubscribeResponse(NPT_Result        res, 
                                        NPT_HttpResponse* response,
                                        PLT_Service*      service,
                                        void*             /* userdata */)
{
    const NPT_String*    sid = NULL;
    NPT_Int32            timeout;
    PLT_EventSubscriber* sub = NULL;

    NPT_AutoLock lock(m_Subscribers);

    NPT_LOG_INFO_2("Received subscription response for service \"%s\" (result = %d)", 
        (const char*)service->GetServiceID(),
        res);
    PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINER, response);

    // if there's a failure or it's a response to a cancellation
    // we get out (any 2xx status code ok)
    if (NPT_FAILED(res) || response == NULL || response->GetStatusCode()/100 != 2) {
        NPT_CHECK_LABEL_SEVERE(NPT_FAILED(res)?res:NPT_FAILURE, failure);
    }
        
    if (!(sid = PLT_UPnPMessageHelper::GetSID(*response)) || 
        NPT_FAILED(PLT_UPnPMessageHelper::GetTimeOut(*response, timeout))) {
        NPT_CHECK_LABEL_SEVERE(NPT_ERROR_INVALID_SYNTAX, failure);
    }

    // look for the subscriber with that sid
    if (NPT_FAILED(NPT_ContainerFind(m_Subscribers, 
                                     PLT_EventSubscriberFinderBySID(*sid), 
                                     sub))) {
        NPT_LOG_INFO_3("Creating new subscriber \"%s\" for service \"%s\" of device \"%s\"",
            (const char*)*sid,
            (const char*)service->GetServiceID(),
            (const char*)service->GetDevice()->GetFriendlyName());

        sub = new PLT_EventSubscriber(&m_TaskManager, service, *sid);
        m_Subscribers.Add(sub);
    }

    sub->SetTimeout(timeout);
    return NPT_SUCCESS;

failure:
    NPT_LOG_SEVERE_3("(un)subscription failed of sub \"%s\" for service \"%s\" of device \"%s\"", 
        (const char*)(sid?*sid:"?"),
        (const char*)service->GetServiceID(),
        (const char*)service->GetDevice()->GetFriendlyName());

    // in case it was a renewal look for the subscriber with that service and remove it from the list
    if (NPT_SUCCEEDED(NPT_ContainerFind(m_Subscribers, 
                                        PLT_EventSubscriberFinderByService(service), 
                                        sub))) {
        m_Subscribers.Remove(sub);
        delete sub;
    }

    return NPT_FAILURE;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::InvokeAction
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::InvokeAction(PLT_ActionReference& action, 
                            void*                userdata)
{
    PLT_Service* service = action->GetActionDesc().GetService();
    
    // create the request
    NPT_HttpUrl url(service->GetControlURL(true));
    NPT_HttpRequest* request = new NPT_HttpRequest(url, "POST", NPT_HTTP_PROTOCOL_1_1);
    
    // create a memory stream for our request body
    NPT_MemoryStreamReference stream(new NPT_MemoryStream);
    action->FormatSoapRequest(*stream);

    // set the request body
    NPT_InputStreamReference input = stream;
    PLT_HttpHelper::SetBody(*request, input);

    PLT_HttpHelper::SetContentType(*request, "text/xml; charset=\"utf-8\"");
    NPT_String service_type = service->GetServiceType();
    NPT_String action_name   = action->GetActionDesc().GetName();
    request->GetHeaders().SetHeader("SOAPAction", "\"" + service_type + "#" + action_name + "\"");

    // create a task to post the request
    PLT_CtrlPointInvokeActionTask* task = new PLT_CtrlPointInvokeActionTask(
        request,
        this, 
        action, 
        userdata);

    // queue the request
    m_TaskManager.StartTask(task);

    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ProcessActionResponse
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ProcessActionResponse(NPT_Result           res, 
                                     NPT_HttpResponse*    response,
                                     PLT_ActionReference& action,
                                     void*                userdata)
{
    NPT_String          service_type;
    NPT_String          str;
    NPT_XmlElementNode* xml = NULL;
    NPT_String          name;
    NPT_String          soap_action_name;
    NPT_XmlElementNode* soap_action_response;
    NPT_XmlElementNode* soap_body;
    NPT_XmlElementNode* fault;
    const NPT_String*   attr = NULL;
    PLT_ActionDesc&     action_desc = action->GetActionDesc();

    // reset the error code and desc
    action->SetError(0, "");

    // check context validity
    if (NPT_FAILED(res) || response == NULL) {
        goto failure;
    }

    NPT_LOG_FINE("Received Action Response:");
    PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINER, response);

    NPT_LOG_FINER("Reading/Parsing Action Response Body...");
    if (NPT_FAILED(PLT_HttpHelper::ParseBody(*response, xml))) {
        goto failure;
    }

    NPT_LOG_FINER("Analyzing Action Response Body...");

    // read envelope
    if (xml->GetTag().Compare("Envelope", true))
        goto failure;

    // check namespace
    if (!xml->GetNamespace() || xml->GetNamespace()->Compare("http://schemas.xmlsoap.org/soap/envelope/"))
        goto failure;

    // check encoding
    attr = xml->GetAttribute("encodingStyle", "http://schemas.xmlsoap.org/soap/envelope/");
    if (!attr || attr->Compare("http://schemas.xmlsoap.org/soap/encoding/"))
        goto failure;

    // read action
    soap_body = PLT_XmlHelper::GetChild(xml, "Body");
    if (soap_body == NULL)
        goto failure;

    // check if an error occurred
    fault = PLT_XmlHelper::GetChild(soap_body, "Fault");
    if (fault != NULL) {
        // we have an error
        ParseFault(action, fault);
        goto failure;
    }

    if (NPT_FAILED(PLT_XmlHelper::GetChild(soap_body, soap_action_response)))
        goto failure;

    // verify action name is identical to SOAPACTION header
    if (soap_action_response->GetTag().Compare(action_desc.GetName() + "Response", true))
        goto failure;

    // verify namespace
    if (!soap_action_response->GetNamespace() ||
         soap_action_response->GetNamespace()->Compare(action_desc.GetService()->GetServiceType()))
         goto failure;

    // read all the arguments if any
    for (NPT_List<NPT_XmlNode*>::Iterator args = soap_action_response->GetChildren().GetFirstItem(); 
         args; 
         args++) {
        NPT_XmlElementNode* child = (*args)->AsElementNode();
        if (!child) continue;

        action->SetArgumentValue(child->GetTag(), child->GetText()?*child->GetText():"");
        if (NPT_FAILED(res)) goto failure; 
    }

    // create a buffer for our response body and call the service
    res = action->VerifyArguments(false);
    if (NPT_FAILED(res)) goto failure; 

    goto cleanup;

failure:
    // override res with failure if necessary
    if (NPT_SUCCEEDED(res)) res = NPT_ERROR_INVALID_FORMAT;
    // fallthrough

cleanup:
    {
        NPT_AutoLock lock(m_ListenerList);
        m_ListenerList.Apply(PLT_CtrlPointListenerOnActionResponseIterator(res, action, userdata));
    }
    
    delete xml;
    return res;
}

/*----------------------------------------------------------------------
|   PLT_CtrlPoint::ParseFault
+---------------------------------------------------------------------*/
NPT_Result
PLT_CtrlPoint::ParseFault(PLT_ActionReference& action,
                          NPT_XmlElementNode*  fault)
{
    NPT_XmlElementNode* detail = fault->GetChild("detail");
    if (detail == NULL) return NPT_FAILURE;

    NPT_XmlElementNode *upnp_error, *error_code, *error_desc;
    upnp_error = detail->GetChild("upnp_error");
	
	// WMP12 Hack
	if (upnp_error == NULL) {
		upnp_error = detail->GetChild("UPnPError", NPT_XML_ANY_NAMESPACE);
    if (upnp_error == NULL) return NPT_FAILURE;
	}

    error_code = upnp_error->GetChild("errorCode", NPT_XML_ANY_NAMESPACE);
    error_desc = upnp_error->GetChild("errorDescription", NPT_XML_ANY_NAMESPACE);
    NPT_Int32  code = 501;    
    NPT_String desc;
    if (error_code && error_code->GetText()) {
        NPT_String value = *error_code->GetText();
        value.ToInteger(code);
    }
    if (error_desc && error_desc->GetText()) {
        desc = *error_desc->GetText();
    }
    action->SetError(code, desc);
    return NPT_SUCCESS;
}
