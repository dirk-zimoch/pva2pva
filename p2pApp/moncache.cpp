
#include <epicsAtomic.h>

#include "helper.h"
#include "pva2pva.h"
#include "chancache.h"

namespace pva = epics::pvAccess;
namespace pvd = epics::pvData;

size_t MonitorCacheEntry::num_instances;
size_t MonitorUser::num_instances;

MonitorCacheEntry::MonitorCacheEntry(ChannelCacheEntry *ent)
    :chan(ent)
    ,done(false)
    ,nwakeups(0)
    ,nevents(0)
{
    epicsAtomicIncrSizeT(&num_instances);
}

MonitorCacheEntry::~MonitorCacheEntry()
{
    pvd::Monitor::shared_pointer M;
    M.swap(mon);
    if(M) {
        M->destroy();
    }
    epicsAtomicDecrSizeT(&num_instances);
}

void
MonitorCacheEntry::monitorConnect(pvd::Status const & status,
                                  pvd::MonitorPtr const & monitor,
                                  pvd::StructureConstPtr const & structure)
{
    assert(monitor==mon);

    interested_t::vector_type tonotify;
    {
        Guard G(mutex());
        typedesc = structure;

        if(status.isSuccess()) {
            startresult = monitor->start();
        } else {
            startresult = status;
        }

        // set typedesc and startresult for futured MonitorUsers
        // and copy snapshot of already interested MonitorUsers
        tonotify = interested.lock_vector();
    }

    if(!startresult.isSuccess())
        std::cout<<"upstream monitor start() fails\n";

    shared_pointer self(weakref); // keeps us alive all MonitorUsers are destroy()ed

    for(interested_t::vector_type::const_iterator it = tonotify.begin(),
        end = tonotify.end(); it!=end; ++it)
    {
        pvd::MonitorRequester::shared_pointer req((*it)->req);
        if(req) {
            req->monitorConnect(startresult, *it, structure);
        } else {
            std::cout<<"Dead requester in monitorConnect()\n";
        }
    }
}

// notificaton from upstream client that its monitor queue has
// become is not empty (transition from empty to not empty)
// will not be called again unless we completely empty the queue.
// If we don't then it is our responsibly to schedule more poll().
void
MonitorCacheEntry::monitorEvent(pvd::MonitorPtr const & monitor)
{
    /* PVA is being tricky, the Monitor* passed to monitorConnect()
     * isn't the same one we see here!
     * The original was a ChannelMonitorImpl, we now see a MonitorStrategyQueue
     * owned by the original, which delegates deserialization and accumulation
     * of deltas into complete events for us.
     * However, we don't want to keep the MonitorStrategyQueue as it's
     * destroy() method is a no-op!
     */
    epicsUInt32 cntpoll = 0;

    //TODO: dequeue and requeue strategy code goes here
    epicsAtomicIncrSizeT(&nwakeups);

    pvd::MonitorElementPtr update;

    shared_pointer self(weakref); // keeps us alive all MonitorUsers are destroy()ed

    while((update=monitor->poll()))
    {
        cntpoll++;
        lastval = update->pvStructurePtr;
        epicsAtomicIncrSizeT(&nevents);

        AUTO_VAL(tonotify, interested.lock_vector()); // TODO: avoid copy, iterate w/ lock

        FOREACH(it, end, tonotify)
        {
            MonitorUser *usr = it->get();
            pvd::MonitorRequester::shared_pointer req(usr->req);

            bool notify = false;
            {
                Guard G(usr->queueLock);
                if(!usr->running || usr->empty.empty()) {
                    epicsAtomicIncrSizeT(&usr->ndropped);
                    continue;
                }
                notify = usr->filled.empty();

                AUTO_VAL(elem, usr->empty.front());

                elem->pvStructurePtr = update->pvStructurePtr;
                elem->overrunBitSet = update->overrunBitSet;
                elem->changedBitSet = update->changedBitSet;

                usr->filled.push_back(elem);
                usr->empty.pop_front();

                epicsAtomicIncrSizeT(&usr->nevents);
            }

            //TODO: notify from worker since monitroEvent() will call poll()
            if(notify) {
                epicsAtomicIncrSizeT(&usr->nwakeups);
                req->monitorEvent(*it); // notify when first item added to empty queue
            }
        }

        monitor->release(update);
    }
}

// notificaton from upstream client that no more monitor updates will come, ever
void
MonitorCacheEntry::unlisten(pvd::MonitorPtr const & monitor)
{
    pvd::Monitor::shared_pointer M;
    M.swap(mon);
    if(M) {
        M->destroy();
        std::cout<<__PRETTY_FUNCTION__<<" destroy client monitor\n";
    }
    // TODO: call all unlisten()
}

std::string
MonitorCacheEntry::getRequesterName()
{
    return "MonitorCacheEntry";
}

void
MonitorCacheEntry::message(std::string const & message, pvd::MessageType messageType)
{
    std::cout<<"message to Monitor cache entry about '"<<chan->channelName<<"' : "<<message<<"\n";
}

MonitorUser::MonitorUser(const MonitorCacheEntry::shared_pointer &e)
    :entry(e)
    ,running(false)
    ,nevents(0)
    ,ndropped(0)
{
    epicsAtomicIncrSizeT(&num_instances);
}

MonitorUser::~MonitorUser()
{
    epicsAtomicDecrSizeT(&num_instances);
}

// downstream server closes monitor
void
MonitorUser::destroy()
{
    {
        Guard G(queueLock);
        running = false;
    }
}

pvd::Status
MonitorUser::start()
{
    pvd::MonitorRequester::shared_pointer req(this->req.lock());
    if(!req)
        return pvd::Status(pvd::Status::STATUSTYPE_FATAL, "already dead");

    bool doEvt = false;
    pvd::PVStructurePtr lval;
    pvd::StructureConstPtr typedesc;
    {
        Guard G(entry->mutex());

        if(!entry->startresult.isSuccess())
            return entry->startresult;

        lval = entry->lastval;
        typedesc = entry->typedesc;
    }

    {
        Guard G(queueLock);

        //TODO: control queue size
        empty.resize(4);
        pvd::PVDataCreatePtr fact(pvd::getPVDataCreate());
        for(unsigned i=0; i<empty.size(); i++) {
            empty[i].reset(new pvd::MonitorElement(fact->createPVStructure(typedesc)));
        }

        if(lval) {
            //already running, notify of initial element
            assert(!empty.empty());

            pvd::MonitorElementPtr elem(empty.front());
            elem->pvStructurePtr = lval;
            elem->changedBitSet->set(0); // indicate all changed?
            filled.push_back(elem);
            empty.pop_front();

            doEvt = true;
        }
        running = true;
    }
    if(doEvt)
        req->monitorEvent(shared_pointer(weakref));
    return pvd::Status();
}

pvd::Status
MonitorUser::stop()
{
    Guard G(queueLock);
    running = false;
    return pvd::Status::Ok;
}

pvd::MonitorElementPtr
MonitorUser::poll()
{
    Guard G(queueLock);
    pvd::MonitorElementPtr ret;
    if(!filled.empty()) {
        ret = filled.front();
        inuse.insert(ret); // track which ones are out for client use
        filled.pop_front();
        //TODO: track lost buffers w/ wrapped shared_ptr?
    }
    return ret;
}

void
MonitorUser::release(pvd::MonitorElementPtr const & monitorElement)
{
    Guard G(queueLock);
    //TODO: ifdef DEBUG? (only track inuse when debugging?)
    std::set<epics::pvData::MonitorElementPtr>::iterator it = inuse.find(monitorElement);
    if(it!=inuse.end()) {
        empty.push_back(monitorElement);
        inuse.erase(it);
    } else {
        // oh no, we've been given an element we weren't expecting
        //TODO: check empty and filled lists to see if this is one of ours, of from somewhere else
        throw std::invalid_argument("Can't release MonitorElement not in use");
    }
    // TODO: pipeline window update
}

std::string
MonitorUser::getRequesterName()
{
    return "MonitorCacheEntry";
}

void
MonitorUser::message(std::string const & message, pvd::MessageType messageType)
{
    std::cout<<"message to Monitor cache client : "<<message<<"\n";
}
