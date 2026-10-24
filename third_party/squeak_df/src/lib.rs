//! Обёртка DeepFilterNet3 для плагина. Отличия от штатного capi libDF:
//! модель вшита, а не читается с диска, и паника внутри tract не пересекает
//! границу FFI — в аудиопотоке APM она уронила бы всё приложение. Вместо неё
//! вызов возвращает ошибку, и плагин переходит на RNNoise.

use std::panic::{catch_unwind, AssertUnwindSafe};

use df::tract::{DfParams, DfTract, ReduceMask, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};

// Ниже этого локального SNR кадр считается чистым шумом и обнуляется. Как в
// deep-filter и штатном capi; RuntimeParams::default берёт −10.
const MIN_DB_THRESH: f32 = -15.0;

// Стадии не пропускаем никогда. С порогом DF-стадии 20 дБ (умолчание
// RuntimeParams) речь поверх клавиатуры выходила с SI-SDR 2–4 дБ вместо 19–27:
// пропуск ломает выравнивание буферов упреждения. Цена та же — модель всё
// равно считается на каждом кадре.
const NEVER_SKIP_DB: f32 = 100.0;

pub struct SqueakDf {
    model: DfTract,
    poisoned: bool,
}

/// Создаёт модель. Медленно (сотни миллисекунд): звать не из аудиопотока.
/// `atten_lim_db` — предел подавления, 100 и больше — без предела.
/// Возвращает NULL, если модель не поднялась.
#[no_mangle]
pub extern "C" fn sqdf_create(atten_lim_db: f32, post_filter_beta: f32) -> *mut SqueakDf {
    let made = catch_unwind(|| {
        let params = RuntimeParams::default_with_ch(1)
            .with_atten_lim(atten_lim_db)
            .with_post_filter(post_filter_beta.max(0.0))
            .with_mask_reduce(ReduceMask::MAX)
            .with_thresholds(MIN_DB_THRESH, NEVER_SKIP_DB, NEVER_SKIP_DB);
        DfTract::new(DfParams::default(), &params).ok()
    });
    match made {
        Ok(Some(model)) => Box::into_raw(Box::new(SqueakDf {
            model,
            poisoned: false,
        })),
        _ => std::ptr::null_mut(),
    }
}

/// Сколько сэмплов за вызов: 480, то есть 10 мс на 48 кГц.
#[no_mangle]
pub unsafe extern "C" fn sqdf_frame_length(st: *const SqueakDf) -> usize {
    st.as_ref().map_or(0, |s| s.model.hop_size)
}

/// Обрабатывает кадр длиной `sqdf_frame_length`, сэмплы в [-1, 1]. `input` и
/// `output` не должны пересекаться. Возвращает локальный SNR кадра в дБ или
/// NaN, если модель сломалась: после этого она только копирует вход.
#[no_mangle]
pub unsafe extern "C" fn sqdf_process(
    st: *mut SqueakDf,
    input: *const f32,
    output: *mut f32,
) -> f32 {
    let Some(state) = st.as_mut() else {
        return f32::NAN;
    };
    let n = state.model.hop_size;
    if state.poisoned {
        std::ptr::copy_nonoverlapping(input, output, n);
        return f32::NAN;
    }
    let noisy = ArrayView2::from_shape_ptr((1, n), input);
    let enh = ArrayViewMut2::from_shape_ptr((1, n), output);
    match catch_unwind(AssertUnwindSafe(|| state.model.process(noisy, enh))) {
        Ok(Ok(lsnr)) => lsnr,
        _ => {
            state.poisoned = true;
            std::ptr::copy_nonoverlapping(input, output, n);
            f32::NAN
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn sqdf_set_atten_lim(st: *mut SqueakDf, atten_lim_db: f32) {
    if let Some(state) = st.as_mut() {
        state.model.set_atten_lim(atten_lim_db);
    }
}

#[no_mangle]
pub unsafe extern "C" fn sqdf_free(st: *mut SqueakDf) {
    if !st.is_null() {
        drop(Box::from_raw(st));
    }
}

