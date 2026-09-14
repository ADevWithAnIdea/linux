// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors

use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::time::{Duration, Instant};
type Result<T = ()> = std::result::Result<T, i32>;
const EIO: i32 = 5;
const ETIMEDOUT: i32 = 110;
fn msecs_to_jiffies(ms: u32) -> u64 {
    ms.into()
}

struct Mutex<T>(std::sync::Mutex<T>);
struct Guard<'a, T>(Option<std::sync::MutexGuard<'a, T>>);
impl<T> Mutex<T> {
    fn lock(&self) -> Guard<'_, T> {
        Guard(Some(self.0.lock().unwrap()))
    }
}
impl<T> std::ops::Deref for Guard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        self.0.as_ref().unwrap()
    }
}
impl<T> std::ops::DerefMut for Guard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        self.0.as_mut().unwrap()
    }
}
enum CondVarTimeoutResult {
    Woken { jiffies: u64 },
    Signal { jiffies: u64 },
    Timeout,
}
struct CondVar {
    inner: std::sync::Condvar,
    waiting: AtomicBool,
}
impl CondVar {
    fn notify_one(&self) {
        self.inner.notify_one();
    }
    fn wait_interruptible_timeout<T>(
        &self,
        guard: &mut Guard<'_, T>,
        jiffies: u64,
    ) -> CondVarTimeoutResult {
        self.waiting.store(true, Ordering::Release);
        let start = Instant::now();
        let (new_guard, result) = self
            .inner
            .wait_timeout(guard.0.take().unwrap(), Duration::from_millis(jiffies))
            .unwrap();
        guard.0 = Some(new_guard);
        if result.timed_out() {
            CondVarTimeoutResult::Timeout
        } else {
            CondVarTimeoutResult::Woken {
                jiffies: jiffies.saturating_sub(start.elapsed().as_millis() as u64),
            }
        }
    }
}
struct Notifications {
    state: Mutex<State>,
    changed: CondVar,
}
fn notifications() -> Arc<Notifications> {
    Arc::new(Notifications {
        state: Mutex(std::sync::Mutex::new(State {
            generation: 0,
            crashed: false,
        })),
        changed: CondVar {
            inner: std::sync::Condvar::new(),
            waiting: AtomicBool::new(false),
        },
    })
}
fn wait_until_sleeping(n: &Notifications) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while !n.changed.waiting.load(Ordering::Acquire) {
        assert!(Instant::now() < deadline);
        std::thread::yield_now();
    }
}
fn main() {
    let barrier = Stamp {
        address: 0xfffffc2200012340,
        value: 0x500,
        event: 34,
    }
    .barrier(0xf00, 0x1234);
    let word = |offset| u32::from_le_bytes(barrier[offset..offset + 4].try_into().unwrap());
    assert_eq!(word(0), 4);
    for offset in [4, 12] {
        assert_eq!(
            u64::from_le_bytes(barrier[offset..offset + 8].try_into().unwrap()),
            0xfffffc2200012340
        );
    }
    for (offset, value) in [
        (0x14, 0x500),
        (0x20, 34),
        (0x24, 0xf00),
        (0x28, 0x1234),
        (0x2c, 0),
        (0x30, 1),
    ] {
        assert_eq!(word(offset), value);
    }
    println!("NOTIFY_PASS general dependency uses dynamic DAG barrier type");

    let n = notifications();
    let generation = n.snapshot().unwrap();
    n.notify(false);
    n.wait(generation, 1000).unwrap();
    assert!(
        !n.changed.waiting.load(Ordering::Acquire),
        "pre-sleep notification was lost"
    );
    println!("NOTIFY_PASS notification before sleep");

    // The notification may race entry to the condvar or reach a sleeping thread.
    for _ in 0..1000 {
        let n = notifications();
        let generation = n.snapshot().unwrap();
        let waiter = n.clone();
        let thread = std::thread::spawn(move || waiter.wait(generation, 1000));
        wait_until_sleeping(&n);
        n.notify(false);
        thread.join().unwrap().unwrap();
    }
    println!("NOTIFY_PASS 1000 concurrent sleep/notify races");

    let n = notifications();
    let generation = n.snapshot().unwrap();
    let waiter = n.clone();
    let thread = std::thread::spawn(move || waiter.wait(generation, 50));
    wait_until_sleeping(&n);
    // Waking without changing the predicate must not report firmware progress.
    for _ in 0..10 {
        n.changed.notify_one();
        std::thread::yield_now();
    }
    assert_eq!(thread.join().unwrap(), Err(ETIMEDOUT));
    println!("NOTIFY_PASS spurious wakeups retain watchdog");

    let n = notifications();
    let generation = n.snapshot().unwrap();
    let waiter = n.clone();
    let thread = std::thread::spawn(move || waiter.wait(generation, 1000));
    wait_until_sleeping(&n);
    n.notify(true);
    assert_eq!(thread.join().unwrap(), Err(EIO));
    n.notify(false);
    assert_eq!(n.snapshot(), Err(EIO));
    assert_eq!(n.wait(generation, 1000), Err(EIO));
    println!("NOTIFY_PASS crash wakes waiter and remains sticky");

    let n = notifications();
    n.state.lock().generation = u64::MAX;
    let generation = n.snapshot().unwrap();
    n.notify(false);
    assert_eq!(n.snapshot(), Ok(0));
    n.wait(generation, 1000).unwrap();
    println!("NOTIFY_PASS generation wrap");
}
