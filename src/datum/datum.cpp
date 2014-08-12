#include <Python.h>

#include <QDebug>

#include "datum/datum.h"
#include "datum/input.h"
#include "datum/link.h"

#include "node/manager.h"

#include "render/render_task.h"

Datum::Datum(QString name, QObject* parent)
    : QObject(parent), value(NULL), valid(false), input_handler(NULL),
      post_init_called(false)
{
    setObjectName(name);
}

Datum::~Datum()
{
    Py_XDECREF(value);
}

bool Datum::hasInputValue() const
{
    return input_handler != NULL && input_handler->hasInput();
}

bool Datum::canEdit() const
{
    return !hasInputValue();
}

bool Datum::acceptsLink(Link* upstream)
{
    if (dynamic_cast<Datum*>(upstream->parent())->_upstream.contains(this))
    {
        return false;
    }
    return input_handler->accepts(upstream);
}

Link* Datum::linkFrom()
{
    return new Link(this);
}

void Datum::addLink(Link* input)
{
    input_handler->addInput(input);
    input->setTarget(this);
    connect(this, SIGNAL(destroyed()), input, SLOT(deleteLater()));
    connect(input, SIGNAL(destroyed()), this, SLOT(update()));

    // For certain types of datums, making a connection changes behavior
    // in a way that requires a changed signal to be emitted.  This is
    // mostly relevant for shape datums, which are only rendered if they
    // are not used elsewhere in the system.
    emit(changed());
    emit(dynamic_cast<Datum*>(input->parent())->changed());
    connect(input, SIGNAL(destroyed()), this, SIGNAL(changed()));
    connect(input, SIGNAL(destroyed()),
            dynamic_cast<Datum*>(input->parent()), SIGNAL(changed()));
}

void Datum::deleteLink(Datum* upstream)
{
    input_handler->deleteInput(upstream);
}

bool Datum::hasConnectedLink() const
{
    for (auto link : findChildren<Link*>())
    {
        if (link->hasTarget())
        {
            return true;
        }
    }
    return false;
}

void Datum::update()
{
    // Prevent recursive calls
    // (at the moment, only of concern for ScriptDatums).
    if (isRecursing())
    {
        return;
    }

    // The very first time that update() is called, refresh all other nodes
    // that may refer to this node by name (then never do so again).
    if (!post_init_called)
    {
        postInit();
    }

    // Request that all upstream datums disconnect.
    emit disconnectFrom(this);
    _upstream.clear();
    _upstream << this;

    PyObject* new_value;
    if (hasInputValue())
    {
        new_value = input_handler->getValue();
    }
    else
    {
        new_value = getCurrentValue();
    }

    bool has_changed = false;
    // If our previous value was valid and our new value is invalid,
    // mark valid = false and emit a changed signal.
    if (new_value == NULL && valid)
    {
        valid = false;
        has_changed = true;
    }
    // If we've gone from invalid to valid or gotten a different object,
    // save the new value and emit changed.
    else if (new_value != NULL && (!valid ||
            PyObject_RichCompareBool(new_value, value, Py_NE)))
    {
        Py_XDECREF(value);
        value = new_value;
        Py_INCREF(value);

        valid = true;

        has_changed = true;
    }
    Py_XDECREF(new_value);

    // If our editable state has changed, mark that we need to emit the
    // changed signal (so that node viewers can modify their lineedits)
    if (canEdit() != editable)
    {
        editable = canEdit();
        has_changed = true;
    }

    // If the string representation has changed, then emit changed
    // so that node viewers can modify their text values.
    if (getString() != repr)
    {
        repr = getString();
        has_changed = true;
    }

    if (has_changed)
    {
        emit changed();
    }
}

void Datum::postInit()
{
    post_init_called = true;
    NodeManager::manager()->onNameChange(objectName());

    // If we're running the antimony app (not the test suite) and this is a
    // function that outputs a shape object, make a RenderTask for it.
#ifdef ANTIMONY
    if (RenderTask::accepts(this))
    {
        new RenderTask(this);
    }
#endif
}

void Datum::onDisconnectRequest(Datum* downstream)
{
    disconnect(downstream, &Datum::disconnectFrom,
               this, &Datum::onDisconnectRequest);
    disconnect(this, &Datum::changed, downstream, &Datum::update);
    disconnect(this, &Datum::destroyed, downstream, &Datum::update);
}

bool Datum::connectUpstream(Datum* upstream)
{
    _upstream << upstream->_upstream;
    connect(upstream, &Datum::changed,
            this, &Datum::update, Qt::UniqueConnection);
    connect(upstream, &Datum::destroyed,
            this, &Datum::update, Qt::UniqueConnection);
    connect(this, &Datum::disconnectFrom,
            upstream, &Datum::onDisconnectRequest, Qt::UniqueConnection);
    return upstream->_upstream.contains(this) ? false : true;
}

QList<Datum*> Datum::getInputDatums() const
{
    return input_handler ? input_handler->getInputDatums()
                         : QList<Datum*>();
}