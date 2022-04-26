// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package proxy

import (
	"bytes"
	"testing"

	"chromiumos/dbusbindings/introspect"
	"chromiumos/dbusbindings/serviceconfig"

	"github.com/google/go-cmp/cmp"
)

func TestGenerateProxies(t *testing.T) {
	itf := introspect.Interface{
		Name: "fi.w1.wpa_supplicant1.Interface",
		Methods: []introspect.Method{
			{
				Name: "Scan",
				Args: []introspect.MethodArg{
					{
						Name: "args",
						Type: "ah",
					},
				},
			}, {
				Name: "PassMeProtos",
				Args: []introspect.MethodArg{
					{
						Name:      "request",
						Type:      "ay",
						Direction: "in",
						Annotation: introspect.Annotation{
							Name:  "org.chromium.DBus.Argument.ProtobufClass",
							Value: "PassMeProtosRequest",
						},
					},
				},
				Annotations: []introspect.Annotation{
					{
						Name:  "org.chromium.DBus.Method.Kind",
						Value: "async",
					},
				},
				DocString: "\n        method doc\n      ",
			},
		},
		Signals: []introspect.Signal{
			{
				Name: "BSSRemoved",
				Args: []introspect.SignalArg{
					{
						Name: "BSSDetail1",
						Type: "ay",
						Annotation: introspect.Annotation{
							Name:  "org.chromium.DBus.Argument.ProtobufClass",
							Value: "YetAnotherProto",
						},
					}, {
						Name: "BSSDetail2",
						Type: "(ih)",
					},
				},
				DocString: "\n        signal doc\n      ",
			},
		},
		Properties: []introspect.Property{
			{
				Name:      "Capabilities",
				Type:      "a{sv}",
				Access:    "read",
				DocString: "\n        property doc\n      ",
			},
		},
		DocString: "\n      interface doc\n    ",
	}

	emptyItf := introspect.Interface{
		Name: "EmptyInterface",
	}

	introspections := []introspect.Introspection{
		{
			Name:       "/org/chromium/Test",
			Interfaces: []introspect.Interface{itf},
		}, {
			Interfaces: []introspect.Interface{emptyItf},
		},
	}

	sc := serviceconfig.Config{
		ObjectManager: &serviceconfig.ObjectManagerConfig{
			Name: "foo.bar.ObjectManager",
		},
	}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - fi.w1.wpa_supplicant1.Interface
//  - EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace foo {
namespace bar {
class ObjectManagerProxy;
}  // namespace bar
}  // namespace foo

namespace fi {
namespace w1 {
namespace wpa_supplicant1 {

// Abstract interface proxy for fi::w1::wpa_supplicant1::Interface.
// interface doc
class InterfaceProxyInterface {
 public:
  virtual ~InterfaceProxyInterface() = default;

  virtual bool Scan(
      const std::vector<brillo::dbus_utils::FileDescriptor>& in_args,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual void ScanAsync(
      const std::vector<brillo::dbus_utils::FileDescriptor>& in_args,
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  // method doc
  virtual bool PassMeProtos(
      const PassMeProtosRequest& in_request,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  // method doc
  virtual void PassMeProtosAsync(
      const PassMeProtosRequest& in_request,
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual void RegisterBSSRemovedSignalHandler(
      const base::RepeatingCallback<void(const YetAnotherProto&,
                                         const std::tuple<int32_t, base::ScopedFD>&)>& signal_callback,
      dbus::ObjectProxy::OnConnectedCallback on_connected_callback) = 0;

  static const char* CapabilitiesName() { return "Capabilities"; }
  virtual const brillo::VariantDictionary& capabilities() const = 0;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;

  virtual void SetPropertyChangedCallback(
      const base::RepeatingCallback<void(InterfaceProxyInterface*, const std::string&)>& callback) = 0;
};

}  // namespace wpa_supplicant1
}  // namespace w1
}  // namespace fi

namespace fi {
namespace w1 {
namespace wpa_supplicant1 {

// Interface proxy for fi::w1::wpa_supplicant1::Interface.
// interface doc
class InterfaceProxy final : public InterfaceProxyInterface {
 public:
  class PropertySet : public dbus::PropertySet {
   public:
    PropertySet(dbus::ObjectProxy* object_proxy,
                const PropertyChangedCallback& callback)
        : dbus::PropertySet{object_proxy,
                            "fi.w1.wpa_supplicant1.Interface",
                            callback} {
      RegisterProperty(CapabilitiesName(), &capabilities);
    }
    PropertySet(const PropertySet&) = delete;
    PropertySet& operator=(const PropertySet&) = delete;

    brillo::dbus_utils::Property<brillo::VariantDictionary> capabilities;

  };
  InterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      PropertySet property_set) :
          bus_{bus},
          service_name_{service_name},
          property_set_{property_set},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  InterfaceProxy(const InterfaceProxy&) = delete;
  InterfaceProxy& operator=(const InterfaceProxy&) = delete;

  ~InterfaceProxy() override {
  }

  void RegisterBSSRemovedSignalHandler(
      const base::RepeatingCallback<void(const YetAnotherProto&,
                                         const std::tuple<int32_t, base::ScopedFD>&)>& signal_callback,
      dbus::ObjectProxy::OnConnectedCallback on_connected_callback) override {
    brillo::dbus_utils::ConnectToSignal(
        dbus_object_proxy_,
        "fi.w1.wpa_supplicant1.Interface",
        "BSSRemoved",
        signal_callback,
        std::move(on_connected_callback));
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

  void SetPropertyChangedCallback(
      const base::RepeatingCallback<void(InterfaceProxyInterface*, const std::string&)>& callback) override {
    on_property_changed_ = callback;
  }

  const PropertySet* GetProperties() const { return &(*property_set_); }
  PropertySet* GetProperties() { return &(*property_set_); }

  bool Scan(
      const std::vector<brillo::dbus_utils::FileDescriptor>& in_args,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "fi.w1.wpa_supplicant1.Interface",
        "Scan",
        error,
        in_args);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error);
  }

  void ScanAsync(
      const std::vector<brillo::dbus_utils::FileDescriptor>& in_args,
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "fi.w1.wpa_supplicant1.Interface",
        "Scan",
        std::move(success_callback),
        std::move(error_callback),
        in_args);
  }

  // method doc
  bool PassMeProtos(
      const PassMeProtosRequest& in_request,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "fi.w1.wpa_supplicant1.Interface",
        "PassMeProtos",
        error,
        in_request);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error);
  }

  // method doc
  void PassMeProtosAsync(
      const PassMeProtosRequest& in_request,
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "fi.w1.wpa_supplicant1.Interface",
        "PassMeProtos",
        std::move(success_callback),
        std::move(error_callback),
        in_request);
  }

  const brillo::VariantDictionary& capabilities() const override {
    return property_set_->capabilities.value();
  }

 private:
  void OnPropertyChanged(const std::string& property_name) {
    if (!on_property_changed_.is_null())
      on_property_changed_.Run(this, property_name);
  }

  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  const dbus::ObjectPath object_path_{"/org/chromium/Test"};
  PropertySet* property_set_;
  base::RepeatingCallback<void(InterfaceProxyInterface*, const std::string&)> on_property_changed_;
  dbus::ObjectProxy* dbus_object_proxy_;
  friend class foo::bar::ObjectManagerProxy;

};

}  // namespace wpa_supplicant1
}  // namespace w1
}  // namespace fi


// Abstract interface proxy for EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;
};



// Interface proxy for EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  class PropertySet : public dbus::PropertySet {
   public:
    PropertySet(dbus::ObjectProxy* object_proxy,
                const PropertyChangedCallback& callback)
        : dbus::PropertySet{object_proxy,
                            "EmptyInterface",
                            callback} {
    }
    PropertySet(const PropertySet&) = delete;
    PropertySet& operator=(const PropertySet&) = delete;


  };
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      const dbus::ObjectPath& object_path) :
          bus_{bus},
          service_name_{service_name},
          object_path_{object_path},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  dbus::ObjectPath object_path_;
  dbus::ObjectProxy* dbus_object_proxy_;

};


#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesEmpty(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
	}

	introspections := []introspect.Introspection{{
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      const dbus::ObjectPath& object_path) :
          bus_{bus},
          service_name_{service_name},
          object_path_{object_path},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  dbus::ObjectPath object_path_;
  dbus::ObjectProxy* dbus_object_proxy_;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesWithServiceName(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
	}

	introspections := []introspect.Introspection{{
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{
		ServiceName: "test.ServiceName",
	}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const dbus::ObjectPath& object_path) :
          bus_{bus},
          object_path_{object_path},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  const std::string service_name_{"test.ServiceName"};
  dbus::ObjectPath object_path_;
  dbus::ObjectProxy* dbus_object_proxy_;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesWithNodeName(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
	}

	introspections := []introspect.Introspection{{
		Name:       "test.node.Name",
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name) :
          bus_{bus},
          service_name_{service_name},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  const dbus::ObjectPath object_path_{"test.node.Name"};
  dbus::ObjectProxy* dbus_object_proxy_;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesWithMethods(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
		Methods: []introspect.Method{{
			Name: "MethodNoArg",
			Args: []introspect.MethodArg{},
		}, {
			Name: "MethodWithInArgs",
			Args: []introspect.MethodArg{
				{Name: "iarg1", Type: "x"},
				{Name: "iarg2", Type: "ay"},
				{Name: "iarg3", Type: "(ih)"},
				{
					Name: "iprotoArg",
					Type: "ay",
					Annotation: introspect.Annotation{
						Name:  "org.chromium.DBus.Argument.ProtobufClass",
						Value: "RequestProto",
					},
				},
			},
		}, {
			Name: "MethodWithOutArgs",
			Args: []introspect.MethodArg{
				{Name: "oarg1", Type: "x", Direction: "out"},
				{Name: "oarg2", Type: "ay", Direction: "out"},
				{Name: "oarg3", Type: "(ih)", Direction: "out"},
				{
					Name:      "oprotoArg",
					Type:      "ay",
					Direction: "out",
					Annotation: introspect.Annotation{
						Name:  "org.chromium.DBus.Argument.ProtobufClass",
						Value: "ResponseProto",
					},
				},
			},
		}, {
			Name: "MethodWithBothArgs",
			Args: []introspect.MethodArg{
				{Name: "iarg1", Type: "x"},
				{Name: "iarg2", Type: "ay"},
				{Name: "oarg1", Type: "q", Direction: "out"},
				{Name: "oarg2", Type: "d", Direction: "out"},
			},
		}, {
			Name: "MethodWithMixedArgs",
			Args: []introspect.MethodArg{
				{Name: "iarg1", Type: "x"},
				{Name: "oarg1", Type: "q", Direction: "out"},
				{Name: "iarg2", Type: "ay"},
				{Name: "oarg2", Type: "d", Direction: "out"},
			},
		}, {
			Name:      "MethodWithDoc",
			DocString: "\n        method doc\n      ",
		}},
	}

	introspections := []introspect.Introspection{{
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  virtual bool MethodNoArg(
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual void MethodNoArgAsync(
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual bool MethodWithInArgs(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      const std::tuple<int32_t, brillo::dbus_utils::FileDescriptor>& in_iarg3,
      const RequestProto& in_iprotoArg,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual void MethodWithInArgsAsync(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      const std::tuple<int32_t, brillo::dbus_utils::FileDescriptor>& in_iarg3,
      const RequestProto& in_iprotoArg,
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual bool MethodWithOutArgs(
      int64_t* out_oarg1,
      std::vector<uint8_t>* out_oarg2,
      std::tuple<int32_t, base::ScopedFD>* out_oarg3,
      ResponseProto* out_oprotoArg,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual void MethodWithOutArgsAsync(
      base::OnceCallback<void(int64_t /*oarg1*/, const std::vector<uint8_t>& /*oarg2*/, const std::tuple<int32_t, base::ScopedFD>& /*oarg3*/, const ResponseProto& /*oprotoArg*/)> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual bool MethodWithBothArgs(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      uint16_t* out_oarg1,
      double* out_oarg2,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual void MethodWithBothArgsAsync(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      base::OnceCallback<void(uint16_t /*oarg1*/, double /*oarg2*/)> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual bool MethodWithMixedArgs(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      uint16_t* out_oarg1,
      double* out_oarg2,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual void MethodWithMixedArgsAsync(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      base::OnceCallback<void(uint16_t /*oarg1*/, double /*oarg2*/)> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  // method doc
  virtual bool MethodWithDoc(
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  // method doc
  virtual void MethodWithDocAsync(
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) = 0;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      const dbus::ObjectPath& object_path) :
          bus_{bus},
          service_name_{service_name},
          object_path_{object_path},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

  bool MethodNoArg(
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodNoArg",
        error);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error);
  }

  void MethodNoArgAsync(
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodNoArg",
        std::move(success_callback),
        std::move(error_callback));
  }

  bool MethodWithInArgs(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      const std::tuple<int32_t, brillo::dbus_utils::FileDescriptor>& in_iarg3,
      const RequestProto& in_iprotoArg,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithInArgs",
        error,
        in_iarg1,
        in_iarg2,
        in_iarg3,
        in_iprotoArg);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error);
  }

  void MethodWithInArgsAsync(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      const std::tuple<int32_t, brillo::dbus_utils::FileDescriptor>& in_iarg3,
      const RequestProto& in_iprotoArg,
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithInArgs",
        std::move(success_callback),
        std::move(error_callback),
        in_iarg1,
        in_iarg2,
        in_iarg3,
        in_iprotoArg);
  }

  bool MethodWithOutArgs(
      int64_t* out_oarg1,
      std::vector<uint8_t>* out_oarg2,
      std::tuple<int32_t, base::ScopedFD>* out_oarg3,
      ResponseProto* out_oprotoArg,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithOutArgs",
        error);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error, out_oarg1, out_oarg2, out_oarg3, out_oprotoArg);
  }

  void MethodWithOutArgsAsync(
      base::OnceCallback<void(int64_t /*oarg1*/, const std::vector<uint8_t>& /*oarg2*/, const std::tuple<int32_t, base::ScopedFD>& /*oarg3*/, const ResponseProto& /*oprotoArg*/)> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithOutArgs",
        std::move(success_callback),
        std::move(error_callback));
  }

  bool MethodWithBothArgs(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      uint16_t* out_oarg1,
      double* out_oarg2,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithBothArgs",
        error,
        in_iarg1,
        in_iarg2);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error, out_oarg1, out_oarg2);
  }

  void MethodWithBothArgsAsync(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      base::OnceCallback<void(uint16_t /*oarg1*/, double /*oarg2*/)> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithBothArgs",
        std::move(success_callback),
        std::move(error_callback),
        in_iarg1,
        in_iarg2);
  }

  bool MethodWithMixedArgs(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      uint16_t* out_oarg1,
      double* out_oarg2,
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithMixedArgs",
        error,
        in_iarg1,
        in_iarg2);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error, out_oarg1, out_oarg2);
  }

  void MethodWithMixedArgsAsync(
      int64_t in_iarg1,
      const std::vector<uint8_t>& in_iarg2,
      base::OnceCallback<void(uint16_t /*oarg1*/, double /*oarg2*/)> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithMixedArgs",
        std::move(success_callback),
        std::move(error_callback),
        in_iarg1,
        in_iarg2);
  }

  // method doc
  bool MethodWithDoc(
      brillo::ErrorPtr* error,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    auto response = brillo::dbus_utils::CallMethodAndBlockWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithDoc",
        error);
    return response && brillo::dbus_utils::ExtractMethodCallResults(
        response.get(), error);
  }

  // method doc
  void MethodWithDocAsync(
      base::OnceCallback<void()> success_callback,
      base::OnceCallback<void(brillo::Error*)> error_callback,
      int timeout_ms = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT) override {
    brillo::dbus_utils::CallMethodWithTimeout(
        timeout_ms,
        dbus_object_proxy_,
        "test.EmptyInterface",
        "MethodWithDoc",
        std::move(success_callback),
        std::move(error_callback));
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  dbus::ObjectPath object_path_;
  dbus::ObjectProxy* dbus_object_proxy_;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesWithSignals(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
		Signals: []introspect.Signal{
			{
				Name: "Signal1",
				Args: []introspect.SignalArg{
					{
						Name: "sarg1_1",
						Type: "ay",
						Annotation: introspect.Annotation{
							Name:  "org.chromium.DBus.Argument.ProtobufClass",
							Value: "YetAnotherProto",
						},
					}, {
						Name: "sarg1_2",
						Type: "(ih)",
					},
				},
				DocString: "\n        signal doc\n      ",
			},
			{
				Name: "Signal2",
				Args: []introspect.SignalArg{
					{
						Name: "sarg2_1",
						Type: "ay",
					}, {
						Name: "sarg2_2",
						Type: "i",
					},
				},
				DocString: "\n        signal doc\n      ",
			},
		},
	}

	introspections := []introspect.Introspection{{
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  virtual void RegisterSignal1SignalHandler(
      const base::RepeatingCallback<void(const YetAnotherProto&,
                                         const std::tuple<int32_t, base::ScopedFD>&)>& signal_callback,
      dbus::ObjectProxy::OnConnectedCallback on_connected_callback) = 0;

  virtual void RegisterSignal2SignalHandler(
      const base::RepeatingCallback<void(const std::vector<uint8_t>&,
                                         int32_t)>& signal_callback,
      dbus::ObjectProxy::OnConnectedCallback on_connected_callback) = 0;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      const dbus::ObjectPath& object_path) :
          bus_{bus},
          service_name_{service_name},
          object_path_{object_path},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void RegisterSignal1SignalHandler(
      const base::RepeatingCallback<void(const YetAnotherProto&,
                                         const std::tuple<int32_t, base::ScopedFD>&)>& signal_callback,
      dbus::ObjectProxy::OnConnectedCallback on_connected_callback) override {
    brillo::dbus_utils::ConnectToSignal(
        dbus_object_proxy_,
        "test.EmptyInterface",
        "Signal1",
        signal_callback,
        std::move(on_connected_callback));
  }

  void RegisterSignal2SignalHandler(
      const base::RepeatingCallback<void(const std::vector<uint8_t>&,
                                         int32_t)>& signal_callback,
      dbus::ObjectProxy::OnConnectedCallback on_connected_callback) override {
    brillo::dbus_utils::ConnectToSignal(
        dbus_object_proxy_,
        "test.EmptyInterface",
        "Signal2",
        signal_callback,
        std::move(on_connected_callback));
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  dbus::ObjectPath object_path_;
  dbus::ObjectProxy* dbus_object_proxy_;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesWithProperties(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
		Properties: []introspect.Property{
			{
				Name:      "ReadonlyProperty",
				Type:      "a{sv}",
				Access:    "read",
				DocString: "\n        property doc\n      ",
			},
			{
				Name:      "WritableProperty",
				Type:      "a{sv}",
				Access:    "readwrite",
				DocString: "\n        property doc\n      ",
			},
		},
	}

	introspections := []introspect.Introspection{{
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  static const char* ReadonlyPropertyName() { return "ReadonlyProperty"; }
  virtual const brillo::VariantDictionary& readonly_property() const = 0;
  static const char* WritablePropertyName() { return "WritableProperty"; }
  virtual const brillo::VariantDictionary& writable_property() const = 0;
  virtual void set_writable_property(const brillo::VariantDictionary& value,
                                     base::OnceCallback<void(bool)> callback) = 0;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;

  virtual void InitializeProperties(
      const base::RepeatingCallback<void(EmptyInterfaceProxyInterface*, const std::string&)>& callback) = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  class PropertySet : public dbus::PropertySet {
   public:
    PropertySet(dbus::ObjectProxy* object_proxy,
                const PropertyChangedCallback& callback)
        : dbus::PropertySet{object_proxy,
                            "test.EmptyInterface",
                            callback} {
      RegisterProperty(ReadonlyPropertyName(), &readonly_property);
      RegisterProperty(WritablePropertyName(), &writable_property);
    }
    PropertySet(const PropertySet&) = delete;
    PropertySet& operator=(const PropertySet&) = delete;

    brillo::dbus_utils::Property<brillo::VariantDictionary> readonly_property;
    brillo::dbus_utils::Property<brillo::VariantDictionary> writable_property;

  };
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      const dbus::ObjectPath& object_path) :
          bus_{bus},
          service_name_{service_name},
          object_path_{object_path},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

  void InitializeProperties(
      const base::RepeatingCallback<void(EmptyInterfaceProxyInterface*, const std::string&)>& callback) override {
    property_set_.reset(
        new PropertySet(dbus_object_proxy_, base::BindRepeating(callback, this)));
    property_set_->ConnectSignals();
    property_set_->GetAll();
  }

  const PropertySet* GetProperties() const { return &(*property_set_); }
  PropertySet* GetProperties() { return &(*property_set_); }

  const brillo::VariantDictionary& readonly_property() const override {
    return property_set_->readonly_property.value();
  }

  const brillo::VariantDictionary& writable_property() const override {
    return property_set_->writable_property.value();
  }

  void set_writable_property(const brillo::VariantDictionary& value,
                             base::OnceCallback<void(bool)> callback) override {
    property_set_->writable_property.Set(value, std::move(callback));
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  dbus::ObjectPath object_path_;
  dbus::ObjectProxy* dbus_object_proxy_;
  std::unique_ptr<PropertySet> property_set_;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesWithObjectManager(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
	}

	introspections := []introspect.Introspection{{
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{
		ObjectManager: &serviceconfig.ObjectManagerConfig{
			Name: "test.ObjectManager",
		},
	}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {
class ObjectManagerProxy;
}  // namespace test

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  class PropertySet : public dbus::PropertySet {
   public:
    PropertySet(dbus::ObjectProxy* object_proxy,
                const PropertyChangedCallback& callback)
        : dbus::PropertySet{object_proxy,
                            "test.EmptyInterface",
                            callback} {
    }
    PropertySet(const PropertySet&) = delete;
    PropertySet& operator=(const PropertySet&) = delete;


  };
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      const dbus::ObjectPath& object_path) :
          bus_{bus},
          service_name_{service_name},
          object_path_{object_path},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

 private:
  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  dbus::ObjectPath object_path_;
  dbus::ObjectProxy* dbus_object_proxy_;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}

func TestGenerateProxiesWithPropertiesAndObjectManager(t *testing.T) {
	emptyItf := introspect.Interface{
		Name: "test.EmptyInterface",
		Properties: []introspect.Property{
			{
				Name:      "Capabilities",
				Type:      "a{sv}",
				Access:    "read",
				DocString: "\n        property doc\n      ",
			},
		},
	}

	introspections := []introspect.Introspection{{
		Interfaces: []introspect.Interface{emptyItf},
	}}

	sc := serviceconfig.Config{
		ObjectManager: &serviceconfig.ObjectManagerConfig{
			Name: "test.ObjectManager",
		},
	}
	out := new(bytes.Buffer)
	if err := Generate(introspections, out, "/tmp/proxy.h", sc); err != nil {
		t.Fatalf("Generate got error, want nil: %v", err)
	}

	const want = `// Automatic generation of D-Bus interfaces:
//  - test.EmptyInterface
#ifndef ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#define ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <brillo/any.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/dbus_property.h>
#include <brillo/dbus/dbus_signal_handler.h>
#include <brillo/dbus/file_descriptor.h>
#include <brillo/errors/error.h>
#include <brillo/variant_dictionary.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace test {
class ObjectManagerProxy;
}  // namespace test

namespace test {

// Abstract interface proxy for test::EmptyInterface.
class EmptyInterfaceProxyInterface {
 public:
  virtual ~EmptyInterfaceProxyInterface() = default;

  static const char* CapabilitiesName() { return "Capabilities"; }
  virtual const brillo::VariantDictionary& capabilities() const = 0;

  virtual const dbus::ObjectPath& GetObjectPath() const = 0;
  virtual dbus::ObjectProxy* GetObjectProxy() const = 0;

  virtual void SetPropertyChangedCallback(
      const base::RepeatingCallback<void(EmptyInterfaceProxyInterface*, const std::string&)>& callback) = 0;
};

}  // namespace test

namespace test {

// Interface proxy for test::EmptyInterface.
class EmptyInterfaceProxy final : public EmptyInterfaceProxyInterface {
 public:
  class PropertySet : public dbus::PropertySet {
   public:
    PropertySet(dbus::ObjectProxy* object_proxy,
                const PropertyChangedCallback& callback)
        : dbus::PropertySet{object_proxy,
                            "test.EmptyInterface",
                            callback} {
      RegisterProperty(CapabilitiesName(), &capabilities);
    }
    PropertySet(const PropertySet&) = delete;
    PropertySet& operator=(const PropertySet&) = delete;

    brillo::dbus_utils::Property<brillo::VariantDictionary> capabilities;

  };
  EmptyInterfaceProxy(
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& service_name,
      const dbus::ObjectPath& object_path,
      PropertySet property_set) :
          bus_{bus},
          service_name_{service_name},
          object_path_{object_path},
          property_set_{property_set},
          dbus_object_proxy_{
              bus_->GetObjectProxy(service_name_, object_path_)} {
  }

  EmptyInterfaceProxy(const EmptyInterfaceProxy&) = delete;
  EmptyInterfaceProxy& operator=(const EmptyInterfaceProxy&) = delete;

  ~EmptyInterfaceProxy() override {
  }

  void ReleaseObjectProxy(base::OnceClosure callback) {
    bus_->RemoveObjectProxy(service_name_, object_path_, std::move(callback));
  }

  const dbus::ObjectPath& GetObjectPath() const override {
    return object_path_;
  }

  dbus::ObjectProxy* GetObjectProxy() const override {
    return dbus_object_proxy_;
  }

  void SetPropertyChangedCallback(
      const base::RepeatingCallback<void(EmptyInterfaceProxyInterface*, const std::string&)>& callback) override {
    on_property_changed_ = callback;
  }

  const PropertySet* GetProperties() const { return &(*property_set_); }
  PropertySet* GetProperties() { return &(*property_set_); }

  const brillo::VariantDictionary& capabilities() const override {
    return property_set_->capabilities.value();
  }

 private:
  void OnPropertyChanged(const std::string& property_name) {
    if (!on_property_changed_.is_null())
      on_property_changed_.Run(this, property_name);
  }

  scoped_refptr<dbus::Bus> bus_;
  std::string service_name_;
  dbus::ObjectPath object_path_;
  PropertySet* property_set_;
  base::RepeatingCallback<void(EmptyInterfaceProxyInterface*, const std::string&)> on_property_changed_;
  dbus::ObjectProxy* dbus_object_proxy_;
  friend class test::ObjectManagerProxy;

};

}  // namespace test

#endif  // ____CHROMEOS_DBUS_BINDING___TMP_PROXY_H
`

	if diff := cmp.Diff(out.String(), want); diff != "" {
		t.Errorf("Generate failed (-got +want):\n%s", diff)
	}
}
