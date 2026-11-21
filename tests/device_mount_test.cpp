#include "browser/browser_controller.h"
#include "fs/drive_monitor.h"
#include "fs/gio_interop.h"
#include "fs/location_mount.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gio/gnativevolumemonitor.h>
#include <print>

namespace {
  // A controllable GIO provider exercises the real request and browser lifetimes without host devices.
  struct DeviceMount {
    GObject parent;
    char* path;
  };
  struct DeviceMountClass {
    GObjectClass parent;
  };
  struct DeviceVolume {
    GObject parent;
    GDrive* drive;
    GMount* mount;
    GTask* task;
    GMountOperation* operation;
    unsigned replies;
  };
  struct DeviceVolumeClass {
    GObjectClass parent;
  };
  struct DeviceMonitor {
    GNativeVolumeMonitor parent;
  };
  struct DeviceMonitorClass {
    GNativeVolumeMonitorClass parent;
  };
  struct DeviceDrive {
    GObject parent;
  };
  struct DeviceDriveClass {
    GObjectClass parent;
  };
  DeviceVolume* currentVolume = nullptr;
  void mountInterface(GMountIface* iface);
  void volumeInterface(GVolumeIface* iface);
  void driveInterface(GDriveIface* iface);
  // GObject requires these generated C entry-point names.
  // NOLINTBEGIN(readability-identifier-naming)
  G_DEFINE_TYPE_WITH_CODE(DeviceMount, device_mount, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_MOUNT, mountInterface))
  G_DEFINE_TYPE_WITH_CODE(
      DeviceVolume, device_volume, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_VOLUME, volumeInterface)
  )
  G_DEFINE_TYPE(DeviceMonitor, device_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)
  G_DEFINE_TYPE_WITH_CODE(DeviceDrive, device_drive, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_DRIVE, driveInterface))
  void device_mount_init(DeviceMount*) {}
  void device_mount_class_init(DeviceMountClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
      g_free(reinterpret_cast<DeviceMount*>(object)->path);
      G_OBJECT_CLASS(device_mount_parent_class)->finalize(object);
    };
  }
  void device_drive_init(DeviceDrive*) {}
  void device_drive_class_init(DeviceDriveClass*) {}
  void device_volume_init(DeviceVolume* volume) {
    volume->drive = G_DRIVE(g_object_new(device_drive_get_type(), nullptr));
  }
  void device_volume_class_init(DeviceVolumeClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
      auto* volume = reinterpret_cast<DeviceVolume*>(object);
      TEST_CHECK(!volume->task && !volume->operation);
      g_clear_object(&volume->mount);
      g_clear_object(&volume->drive);
      G_OBJECT_CLASS(device_volume_parent_class)->finalize(object);
    };
  }
  void device_monitor_init(DeviceMonitor*) {}
  void device_monitor_class_init(DeviceMonitorClass* klass) {
    auto* monitor = G_VOLUME_MONITOR_CLASS(klass);
    monitor->is_supported = +[] -> gboolean { return TRUE; };
    monitor->get_volumes = +[](GVolumeMonitor*) -> GList* {
      return currentVolume ? g_list_append(nullptr, g_object_ref(currentVolume)) : nullptr;
    };
    monitor->get_mounts = +[](GVolumeMonitor*) -> GList* { return nullptr; };
    monitor->get_connected_drives = +[](GVolumeMonitor*) -> GList* { return nullptr; };
  }
  // NOLINTEND(readability-identifier-naming)

  void driveInterface(GDriveIface* iface) {
    iface->is_removable = +[](GDrive*) -> gboolean { return TRUE; };
    iface->can_stop = +[](GDrive*) -> gboolean { return FALSE; };
  }

  void mountInterface(GMountIface* iface) {
    iface->get_root = +[](GMount* mount) { return g_file_new_for_path(reinterpret_cast<DeviceMount*>(mount)->path); };
    iface->get_name = +[](GMount*) { return g_strdup("Disposable unlocked drive"); };
    iface->get_drive = +[](GMount*) -> GDrive* { return nullptr; };
    iface->can_unmount = +[](GMount*) -> gboolean { return FALSE; };
    iface->can_eject = +[](GMount*) -> gboolean { return FALSE; };
  }
  void ask(DeviceVolume* volume) {
    g_signal_emit_by_name(
        volume->operation, "ask-password", "Enter the passphrase for Disposable <LUKS> café", "", "",
        static_cast<GAskPasswordFlags>(G_ASK_PASSWORD_NEED_PASSWORD | G_ASK_PASSWORD_SAVING_SUPPORTED)
    );
  }
  void finish(DeviceVolume* volume, bool success, GIOErrorEnum failure = G_IO_ERROR_CANCELLED) {
    auto* task = std::exchange(volume->task, nullptr);
    TEST_CHECK(task);
    g_signal_handlers_disconnect_by_data(volume->operation, volume);
    g_clear_object(&volume->operation);
    if (success) {
      g_task_return_boolean(task, TRUE);
    } else {
      g_task_return_new_error(task, G_IO_ERROR, failure, "Fixture provider error");
    }
    g_object_unref(task);
  }
  void volumeInterface(GVolumeIface* iface) {
    iface->get_name = +[](GVolume*) { return g_strdup("Disposable LUKS drive"); };
    iface->get_identifier = +[](GVolume*, const char*) { return g_strdup("luciole-test-device"); };
    iface->get_drive = +[](GVolume* source) -> GDrive* {
      return G_DRIVE(g_object_ref(reinterpret_cast<DeviceVolume*>(source)->drive));
    };
    iface->get_mount = +[](GVolume* source) -> GMount* {
      auto* volume = reinterpret_cast<DeviceVolume*>(source);
      return volume->mount ? G_MOUNT(g_object_ref(volume->mount)) : nullptr;
    };
    iface->mount_fn = +[](GVolume* source, GMountMountFlags, GMountOperation* operation, GCancellable* token,
                          GAsyncReadyCallback callback, gpointer data) {
      auto* volume = reinterpret_cast<DeviceVolume*>(source);
      TEST_CHECK(!volume->task);
      volume->task = g_task_new(source, token, callback, data);
      // Deliberately allow a late success to verify the consumer rejects obsolete requests itself.
      g_task_set_check_cancellable(volume->task, FALSE);
      volume->operation = G_MOUNT_OPERATION(g_object_ref(operation));
      g_signal_connect(
          operation, "reply", G_CALLBACK(+[](GMountOperation*, GMountOperationResult result, gpointer data) {
            auto* volume = static_cast<DeviceVolume*>(data);
            ++volume->replies;
            if (result == G_MOUNT_OPERATION_HANDLED) {
              if (std::string_view(g_mount_operation_get_password(volume->operation)) == "fixture-secret") {
                finish(volume, true);
              } else {
                ask(volume);
              }
            } else {
              finish(volume, false);
            }
          }),
          volume
      );
      ask(volume);
    };
    iface->mount_finish =
        +[](GVolume*, GAsyncResult* result, GError** error) { return g_task_propagate_boolean(G_TASK(result), error); };
  }
  void pump(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      g_main_context_iteration(nullptr, FALSE);
    }
  }
  struct Fixture {
    luciole::GioPtr<GVolume> volume{G_VOLUME(g_object_new(device_volume_get_type(), nullptr))};
    luciole::GString path{g_dir_make_tmp("luciole-device-XXXXXX", nullptr)};
    Fixture() {
      TEST_CHECK(path);
      currentVolume = reinterpret_cast<DeviceVolume*>(volume.get());
      std::ofstream(std::filesystem::path(path.get()) / "canary.txt") << "disposable exact contents\n";
    }
    ~Fixture() {
      TEST_CHECK(!currentVolume->task);
      currentVolume = nullptr;
      std::filesystem::remove_all(path.get());
    }
    void mounted() const {
      auto* mount = reinterpret_cast<DeviceMount*>(g_object_new(device_mount_get_type(), nullptr));
      mount->path = g_strdup(path.get());
      g_clear_object(&currentVolume->mount);
      currentVolume->mount = G_MOUNT(mount);
    }
  };
  void unlockAndRetry() {
    Fixture fixture;
    luciole::LocationMount request;
    bool done = false;
    request.startVolume(fixture.volume.get(), [&](const auto& result) {
      TEST_CHECK(result && *result == luciole::Location::fromPath(fixture.path.get()));
      done = true;
    });
    TEST_CHECK(request.interaction().prompt()->purpose == luciole::MountPurpose::Device);
    const auto first = request.interaction().prompt()->id;
    // LUKS providers can replace the encrypted volume while retaining the physical drive and mount request.
    g_signal_emit_by_name(fixture.volume.get(), "removed");
    TEST_CHECK(request.interaction().prompt() && request.interaction().prompt()->id == first);
    TEST_CHECK(request.interaction().answer(first, {"", "", "wrong", false}));
    TEST_CHECK(request.interaction().prompt() && request.interaction().prompt()->id != first);
    TEST_CHECK(!request.interaction().answer(first, {"", "", "fixture-secret", false}));
    fixture.mounted();
    TEST_CHECK(request.interaction().answer(request.interaction().prompt()->id, {"", "", "fixture-secret", false}));
    pump([&] { return done; });
    TEST_CHECK(!request.interaction().prompt());
  }
  void cancellationAndRemoval() {
    Fixture fixture;
    luciole::LocationMount request;
    bool done = false;
    request.startVolume(fixture.volume.get(), [&](const auto& result) {
      TEST_CHECK(!result && result.error().kind == luciole::FileErrorKind::Cancelled);
      done = true;
    });
    request.interaction().cancel();
    pump([&] { return done; });
    TEST_CHECK(!request.interaction().prompt());
    done = false;
    request.startVolume(fixture.volume.get(), [&](const auto& result) {
      TEST_CHECK(!result && result.error().kind == luciole::FileErrorKind::Cancelled);
      done = true;
    });
    g_signal_emit_by_name(currentVolume->drive, "disconnected");
    TEST_CHECK(!request.interaction().prompt());
    pump([&] { return done; });
  }
  void obsoleteAndDestroyed() {
    Fixture fixture;
    luciole::LocationMount request;
    bool obsolete = false, done = false;
    request.startVolume(fixture.volume.get(), [&](const auto&) { obsolete = true; });
    // A provider that has stopped asking but has not yet completed.
    g_signal_emit_by_name(currentVolume->operation, "aborted");
    request.cancel();
    finish(currentVolume, true);
    request.start(luciole::Location::fromPath(fixture.path.get()), [&](const auto&) { done = true; });
    pump([&] { return done; });
    TEST_CHECK(!obsolete);
    auto destroyed = std::make_unique<luciole::LocationMount>();
    destroyed->startVolume(fixture.volume.get(), [&](const auto&) { obsolete = true; });
    destroyed.reset();
    done = false;
    request.start(luciole::Location::fromPath(fixture.path.get()), [&](const auto&) { done = true; });
    pump([&] { return done; });
    TEST_CHECK(!obsolete);
  }
  void providerFailuresAndMountedRace() {
    Fixture fixture;
    luciole::LocationMount request;
    bool done = false;
    request.startVolume(fixture.volume.get(), [&](const auto& result) {
      TEST_CHECK(!result && result.error().kind == luciole::FileErrorKind::Other);
      done = true;
    });
    // A reported success without a mounted root must never open File System as a fallback.
    TEST_CHECK(request.interaction().answer(request.interaction().prompt()->id, {"", "", "fixture-secret", false}));
    pump([&] { return done; });
    done = false;
    request.startVolume(fixture.volume.get(), [&](const auto& result) {
      TEST_CHECK(!result && result.error().kind == luciole::FileErrorKind::Denied);
      done = true;
    });
    finish(currentVolume, false, G_IO_ERROR_PERMISSION_DENIED);
    pump([&] { return done; });
    TEST_CHECK(!request.interaction().prompt());
    done = false;
    request.startVolume(fixture.volume.get(), [&](const auto& result) {
      TEST_CHECK(result && *result == luciole::Location::fromPath(fixture.path.get()));
      done = true;
    });
    fixture.mounted();
    finish(currentVolume, false, G_IO_ERROR_ALREADY_MOUNTED);
    pump([&] { return done; });
    done = false;
    request.startVolume(fixture.volume.get(), [&](const auto& result) {
      TEST_CHECK(!result && result.error().kind == luciole::FileErrorKind::Unsupported);
      done = true;
    });
    g_signal_emit_by_name(
        currentVolume->operation, "ask-password", "Unsupported", "", "", static_cast<GAskPasswordFlags>(1U << 20)
    );
    pump([&] { return done; });
  }
  void browserAndDriveDispatch() {
    Fixture fixture;
    luciole::DriveMonitor drives;
    drives.start();
    luciole::BrowserController browser({}, false);
    const auto home = luciole::Location::fromPath(fixture.path.get());
    browser.open(home);
    bool obsolete = false;
    browser.openDrive(drives, "volume:luciole-test-device", [&](const auto&) { obsolete = true; });
    TEST_CHECK(browser.mountInteraction()->prompt());
    browser.open(luciole::Location::fromPath("/"));
    TEST_CHECK(!browser.mountInteraction()->prompt());
    pump([&] { return browser.model().snapshot().status == luciole::DirectoryStatus::Ready; });
    TEST_CHECK(!obsolete && browser.model().snapshot().location == luciole::Location::fromPath("/"));
    bool done = false;
    browser.openDrive(drives, "volume:luciole-test-device", [&](const std::string& error) {
      TEST_CHECK(error.empty());
      done = true;
    });
    fixture.mounted();
    auto* interaction = browser.mountInteraction();
    TEST_CHECK(interaction->answer(interaction->prompt()->id, {"", "", "fixture-secret", false}));
    pump([&] { return done && browser.model().snapshot().status == luciole::DirectoryStatus::Ready; });
    TEST_CHECK(browser.model().snapshot().location == home);
    TEST_CHECK(browser.model().snapshot().entries.size() == 1);
    done = false;
    browser.openDrive(drives, "missing-device", [&](const std::string& error) {
      TEST_CHECK(error == "This drive is no longer available.");
      done = true;
    });
    TEST_CHECK(done && browser.model().snapshot().location == home);
  }
  void completionCanDestroyBrowser() {
    luciole::DriveMonitor drives;
    auto browser = std::make_unique<luciole::BrowserController>(luciole::SortOptions{}, false);
    browser->openDrive(drives, "missing-device", [&](const std::string& error) {
      TEST_CHECK(!error.empty());
      browser.reset();
    });
    TEST_CHECK(!browser);
  }
} // namespace

int main() {
  g_io_extension_point_implement(
      G_NATIVE_VOLUME_MONITOR_EXTENSION_POINT_NAME, device_monitor_get_type(), "luciole-test", 10000
  );
  g_setenv("GIO_USE_VOLUME_MONITOR", "luciole-test", TRUE);
  unlockAndRetry();
  cancellationAndRemoval();
  obsoleteAndDestroyed();
  providerFailuresAndMountedRace();
  browserAndDriveDispatch();
  completionCanDestroyBrowser();
  std::println("Device unlock, retry, cancellation, removal and browser lifetime checks passed");
}
