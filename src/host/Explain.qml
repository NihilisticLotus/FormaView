pragma Singleton
import QtQuick
// User-facing wording for conversion losses, errors and notes. Producers keep
// stable English codes/messages (needed by MCP and logs); this singleton turns
// them into Simplified Chinese, English or Traditional Chinese explanations.
// Technical text is never discarded: callers show it as a copyable second line.
QtObject {
    // {d} is replaced by the producer's detail (a name, key or count).
    readonly property var losses: ({
        "selected_subresource": ["只写出所选的条目/部件/Mip/面/层/切片；其他子资源不会进入输出。", "Only the selected entry/part/mip/face/layer/slice is written; other sub-images are not in the output.", "只寫出所選的項目/部件/Mip/面/層/切片；其他子資源不會進入輸出。"],
        "omitted_channel": ["源通道 {d} 没有写入输出。", "Source channel {d} is not written to the output.", "來源通道 {d} 沒有寫入輸出。"],
        "alpha_removed": ["透明度（Alpha）已按所选策略移除或合成到背景，输出不再含透明信息。", "Alpha was removed or composited by the chosen policy; the output has no transparency.", "透明度（Alpha）已依所選策略移除或合成至背景，輸出不再含透明資訊。"],
        "clipped_samples": ["有 {d} 个采样值超出目标范围，已被截断到可表示的范围。", "{d} samples were outside the target range and were clamped.", "有 {d} 個取樣值超出目標範圍，已被截斷至可表示範圍。"],
        "precision_changed": ["有 {d} 个采样值因目标位深更低而改变了精度。", "{d} samples changed precision because the target bit depth is lower.", "有 {d} 個取樣值因目標位元深度較低而改變精度。"],
        "non_finite_replaced": ["有 {d} 个非有限值（NaN/无穷）已按设置替换。", "{d} non-finite values (NaN/Inf) were replaced as configured.", "有 {d} 個非有限值（NaN/無限大）已依設定取代。"],
        "colour_transform": ["已对颜色采样应用曝光/传递函数/合成设置，输出像素与源像素不同。", "Exposure, transfer or compositing settings were applied; output pixels differ from the source.", "已對顏色取樣套用曝光/傳遞函數/合成設定，輸出像素與來源像素不同。"],
        "window_metadata": ["只保留数据窗口内的像素；原始位置与显示窗口信息被丢弃。", "Only data-window pixels are kept; original placement and display window are discarded.", "只保留資料視窗內的像素；原始位置與顯示視窗資訊被捨棄。"],
        "icc_profile": ["源 ICC 色彩配置文件不会复制到输出。", "The source ICC colour profile is not copied.", "來源 ICC 色彩設定檔不會複製到輸出。"],
        "metadata_field": ["元数据未复制：{d}", "Metadata not copied: {d}", "中繼資料未複製：{d}"],
        "part_name": ["容器的部件/条目名称不会保留。", "The container part/entry name is not retained.", "容器的部件/項目名稱不會保留。"],
        "padding_channel": ["DXGI 存储会补一个不透明的填充分量，它不是源图的 Alpha 通道。", "DXGI storage adds an opaque padding component; it is not a source alpha channel.", "DXGI 儲存會補一個不透明的填充分量，它不是來源圖的 Alpha 通道。"],
        "lossy_codec": ["有损编码：{d}，重新编码会损失部分画质。", "Lossy encoding ({d}); re-encoding reduces quality.", "有損編碼：{d}，重新編碼會損失部分畫質。"],
        "actorx_chunk_omitted": ["ActorX 数据块 {d} 无法写入 FBX，已省略。", "ActorX chunk {d} cannot be written to FBX and is omitted.", "ActorX 資料區塊 {d} 無法寫入 FBX，已省略。"],
        "actorx_material_slots": ["保留 ActorX 材质名称与槽位；源文件不含已解析的着色或贴图网络。", "ActorX material names and slots are kept; the source has no resolved shading or texture graph.", "保留 ActorX 材質名稱與槽位；來源檔不含已解析的著色或貼圖網路。"],
        "actorx_material_flags": ["ActorX 引擎材质标志不是 FBX 着色语义，不会写出。", "ActorX engine material flags are not FBX shading semantics and are not written.", "ActorX 引擎材質旗標不是 FBX 著色語意，不會寫出。"],
        "actorx_auxiliary_material": ["辅助材质索引不会导出。", "Auxiliary material indices are not exported.", "輔助材質索引不會匯出。"],
        "psa_companion_unmatched_rule": ["PSA 同名 .config 中有规则没有匹配到任何轨道：{d}", "A rule in the PSA .config matched no track: {d}", "PSA 同名 .config 中有規則沒有比對到任何軌道：{d}"],
        "external_texture_unavailable": ["贴图无法读取或找不到，输出将缺少该贴图：{d}", "A texture could not be read or found; the output will lack it: {d}", "貼圖無法讀取或找不到，輸出將缺少該貼圖：{d}"],
        "morph_mesh_omitted": ["“只导出动画”不包含网格，基于网格的形态键（Blend Shape）被省略。", "Animation-only output has no mesh, so mesh-based blend shapes are omitted.", "「只匯出動畫」不含網格，基於網格的形態鍵（Blend Shape）被省略。"],
        "gltf_texture_rotation": ["KHR_texture_transform 的旋转仅作为元数据保留；不同 FBX 导入器处理贴图旋转的方式不一。", "KHR_texture_transform rotation is kept only as metadata; FBX importers handle texture rotation differently.", "KHR_texture_transform 的旋轉僅作為中繼資料保留；不同 FBX 匯入器處理貼圖旋轉的方式不一。"],
        "gltf_mirrored_repeat": ["FBX 贴图环绕只支持重复，镜像重复会改为普通重复。", "FBX texture wrapping supports repeat only; mirrored repeat becomes plain repeat.", "FBX 貼圖環繞只支援重複，鏡像重複會改為一般重複。"],
        "gltf_sampler_filter": ["glTF 采样器的放大/缩小过滤方式在 FBX 中没有通用对应，不会保留。", "glTF sampler filters have no portable FBX equivalent and are not kept.", "glTF 取樣器的放大/縮小過濾方式在 FBX 中沒有通用對應，不會保留。"],
        "gltf_pbr_shading": ["保留基础色、法线/自发光贴图和材质槽；金属度/粗糙度/透明度/剔除等 PBR 语义只作元数据，FBX Phong 无法还原 glTF 的 PBR 着色。", "Base colour, normal/emission textures and material slots are kept; metallic/roughness/alpha/culling stay as metadata. FBX Phong cannot reproduce glTF PBR shading.", "保留基礎色、法線/自發光貼圖與材質槽；金屬度/粗糙度/透明度/剔除等 PBR 語意只作中繼資料，FBX Phong 無法還原 glTF 的 PBR 著色。"],
        "gltf_material_extensions": ["glTF 高级材质扩展（如清漆、镜面光泽等）不会在 FBX Phong 中还原。", "Advanced glTF material extensions (clearcoat, specular-glossiness, etc.) are not reproduced in FBX Phong.", "glTF 進階材質擴充（如清漆、鏡面光澤等）不會在 FBX Phong 中還原。"],
        "gltf_camera_light": ["相机和灯光对象被省略，它们的变换节点仍保留。", "Camera and light objects are omitted; their transform nodes are kept.", "相機與燈光物件被省略，其變換節點仍保留。"],
        "gltf_multiple_scenes": ["所有节点导出到同一个 FBX 场景，不保留 glTF 的场景选择。", "All nodes are exported into one FBX scene; glTF scene selection is not kept.", "所有節點匯出至同一個 FBX 場景，不保留 glTF 的場景選擇。"],
        "gltf_custom_attribute": ["自定义顶点属性 {d} 没有对应的 FBX 语义，已省略。", "Custom vertex attribute {d} has no FBX equivalent and is omitted.", "自訂頂點屬性 {d} 沒有對應的 FBX 語意，已省略。"],
        "gltf_weight_normalization": ["保留源权重数值；FBX 蒙皮在变形时会把非 1 的权重和归一化。", "Source weights are kept; FBX skinning normalizes weight sums that are not 1 during deformation.", "保留來源權重數值；FBX 蒙皮在變形時會將非 1 的權重和正規化。"],
        "gltf_morph_tangents": ["保留形态键的位置与法线偏移；切线偏移被省略。", "Morph position and normal deltas are kept; tangent deltas are omitted.", "保留形態鍵的位置與法線位移；切線位移被省略。"],
        "vrm_semantics": ["VRM 扩展：几何、蒙皮与形态键会导出；人形映射、表情、弹簧物理、视线约束与 MToon 运行时行为在 FBX 中没有等价物。", "VRM extension: geometry, skin and morphs are exported; humanoid mapping, expressions, spring physics, look-at constraints and MToon runtime behaviour have no FBX equivalent.", "VRM 擴充：幾何、蒙皮與形態鍵會匯出；人形對應、表情、彈簧物理、視線約束與 MToon 執行階段行為在 FBX 中沒有等價物。"],
        "gltf_extension": ["glTF 扩展 {d} 在 FBX 中没有对应，不会保留。", "glTF extension {d} has no FBX equivalent and is not kept.", "glTF 擴充 {d} 在 FBX 中沒有對應，不會保留。"],
        "gltf_animation_bake": ["四元数与三次样条动画会自适应烘焙为 FBX 欧拉/线性曲线（局部角度/相对分量容差 1e-5）；原片段时间和独立动作栈保留。", "Quaternion and cubic-spline animation is adaptively baked to FBX Euler/linear curves (tolerance 1e-5); original clip times and independent stacks are kept.", "四元數與三次樣條動畫會自適應烘焙為 FBX 歐拉/線性曲線（區域角度/相對分量容差 1e-5）；原片段時間與獨立動作堆疊保留。"],
        "obj_texture_options": ["OBJ/MTL 贴图选项不会保留：{d}", "OBJ/MTL texture options are not kept: {d}", "OBJ/MTL 貼圖選項不會保留：{d}"],
        "obj_material_property": ["MTL 材质属性 {d} 没有对应的 FBX 着色参数，已忽略。", "MTL material property {d} has no FBX shading equivalent and is ignored.", "MTL 材質屬性 {d} 沒有對應的 FBX 著色參數，已忽略。"],
        "obj_uv_third_component": ["FBX 的 UV 集只保留前两个分量。", "FBX UV sets keep only the first two components.", "FBX 的 UV 集只保留前兩個分量。"],
        "obj_smoothing_groups": ["编号大于 31 的平滑组无法用 FBX 位掩码表示；显式法线仍保留。", "Smoothing groups above 31 cannot be FBX bit masks; explicit normals are kept.", "編號大於 31 的平滑群組無法用 FBX 位元遮罩表示；明確法線仍保留。"],
        "obj_grouping": ["OBJ 的对象/组标签被合并为一个网格；多边形、材质分配和角点属性保留。", "OBJ object/group labels are merged into one mesh; polygons, material assignments and corner attributes are kept.", "OBJ 的物件/群組標籤合併為一個網格；多邊形、材質指派與角點屬性保留。"],
        "obj_missing_material": ["面引用的材质 {d} 在 MTL 中不存在，已用默认材质。", "Material {d} used by faces is missing from the MTL; a default material is used.", "面所參照的材質 {d} 在 MTL 中不存在，已使用預設材質。"],
        "obj_missing_corner_normals": ["部分角点缺少法线，写为零向量；已有的源法线保留。", "Some corners have no normal and are written as zero; existing source normals are kept.", "部分角點缺少法線，寫為零向量；已有的來源法線保留。"],
        "obj_missing_corner_uv": ["部分角点缺少 UV，使用 (0,0)；已有的源 UV 保留。", "Some corners have no UV and use (0,0); existing source UVs are kept.", "部分角點缺少 UV，使用 (0,0)；已有的來源 UV 保留。"],
        "obj_coordinate_assumption": ["OBJ 不声明坐标轴与单位：按右手系、Y 轴向上、单位米解释并标注，数值本身不变（这是约定，不是推断）。", "OBJ declares no axes or units: numbers are kept and tagged right-handed, Y-up, metres (a convention, not an inference).", "OBJ 不宣告座標軸與單位：依右手系、Y 軸向上、單位公尺解讀並標註，數值本身不變（這是約定，不是推斷）。"],
        "pmx_sdef": ["PMX SDEF 控制向量在 FBX 中没有对应；原有的两个骨骼影响改为线性蒙皮导出。", "PMX SDEF control vectors have no FBX equivalent; the two bone influences are exported as linear skinning.", "PMX SDEF 控制向量在 FBX 中沒有對應；原有的兩個骨骼影響改為線性蒙皮匯出。"],
        "pmx_edge_scale": ["逐顶点的描边缩放（卡通轮廓）不会写入 FBX 着色。", "Per-vertex toon edge scale is not written to FBX shading.", "逐頂點的描邊縮放（卡通輪廓）不會寫入 FBX 著色。"],
        "pmx_material_semantics": ["卡通/球面贴图、描边、阴影与剔除标志不会由 FBX Phong 还原；基础色、透明度、高光、漫反射贴图与材质分配保留。", "Toon/sphere shading, edges, shadow and culling flags are not reproduced by FBX Phong; base colour, alpha, specular, diffuse textures and material assignments are kept.", "卡通/球面貼圖、描邊、陰影與剔除旗標不會由 FBX Phong 還原；基礎色、透明度、高光、漫反射貼圖與材質指派保留。"],
        "pmx_bone_controls": ["PMX 的 IK、追加变换、局部/固定轴、外部父级与物理求值顺序被省略；骨骼层级与静止/绑定位置保留。", "PMX IK, append transforms, local/fixed axes, external parents and physics order are omitted; hierarchy and rest/bind positions are kept.", "PMX 的 IK、附加變換、區域/固定軸、外部父級與物理求值順序被省略；骨骼層級與靜止/綁定位置保留。"],
        "pmx_weight_normalization": ["保留原始影响值；FBX 蒙皮求值会把非 1 的权重和归一化。", "Raw influence values are kept; FBX skin evaluation normalizes sums that are not 1.", "保留原始影響值；FBX 蒙皮求值會將非 1 的權重和正規化。"],
        "pmx_nonvertex_morph": ["{d}（骨骼/UV/材质/翻转/冲量类表情）没有等价的 FBX 顶点混合形状，已省略。", "{d} (bone/UV/material/flip/impulse morph) has no equivalent FBX vertex blend shape and is omitted.", "{d}（骨骼/UV/材質/翻轉/衝量類表情）沒有等價的 FBX 頂點混合形狀，已省略。"],
        "pmx_partial_group_morph": ["{d}（组表情）只转换其中的顶点位移成员。", "{d} (group morph): only vertex-displacement members are converted.", "{d}（群組表情）只轉換其中的頂點位移成員。"],
        "pmx_display_frames": ["MMD 显示框组织被省略。", "MMD display-frame organization is omitted.", "MMD 顯示框組織被省略。"],
        "pmx_physics": ["刚体与物理关节被省略；FBX 只含静止模型与蒙皮，不含 MMD 物理模拟。", "Rigid bodies and physics joints are omitted; FBX holds the rest model and skin without MMD simulation.", "剛體與物理關節被省略；FBX 只含靜止模型與蒙皮，不含 MMD 物理模擬。"],
        "pmx_soft_bodies": ["PMX 软体模拟被省略。", "PMX soft-body simulation is omitted.", "PMX 軟體模擬被省略。"],
        "pmx_unit_assumption": ["PMX 不声明物理尺度：按左手系、Y 轴向上、单位米解释并标注，数值本身不变（这是约定，不是推断）。", "PMX declares no physical scale: numbers are kept and tagged left-handed, Y-up, metres (a convention, not an inference).", "PMX 不宣告物理尺度：依左手系、Y 軸向上、單位公尺解讀並標註，數值本身不變（這是約定，不是推斷）。"]
    })

    // Error-code headline: what happened and what to do.
    readonly property var errorCodes: ({
        "unsupported": ["不支持此内容或设置。", "This content or setting is not supported.", "不支援此內容或設定。"],
        "invalid_argument": ["输入或参数无效，请检查后重试。", "An input or parameter is invalid; check it and try again.", "輸入或參數無效，請檢查後重試。"],
        "queue_full": ["任务队列已满，请等待现有任务结束后重试。", "The task queue is full; wait for current tasks to finish and retry.", "工作佇列已滿，請等待現有工作結束後重試。"],
        "resource_limit": ["超出大小或资源限制，未处理。", "A size or resource limit was exceeded; nothing was processed.", "超出大小或資源限制，未處理。"],
        "shutting_down": ["应用正在关闭，任务未执行。", "The application is closing; the task was not run.", "應用程式正在關閉，工作未執行。"],
        "cancelled": ["操作已取消，未完成的输出不会保留。", "The operation was cancelled; unfinished output is not kept.", "操作已取消，未完成的輸出不會保留。"],
        "stale_result": ["结果已被新的请求替换，已丢弃。", "The result was replaced by a newer request and discarded.", "結果已被較新的請求取代並捨棄。"],
        "input_changed": ["源文件、参数或授权范围已变化，请重新预检后再提交。", "The source, parameters or scope changed; run a new preflight before submitting.", "來源檔、參數或授權範圍已變化，請重新預檢後再提交。"],
        "out_of_scope": ["路径不在已授权的范围内，操作被拒绝。", "The path is outside the authorized scope; the operation was refused.", "路徑不在已授權的範圍內，操作被拒絕。"],
        "conflict": ["输出位置冲突（目标已存在或被占用），未写入。", "Output location conflict (target exists or is in use); nothing was written.", "輸出位置衝突（目標已存在或被占用），未寫入。"],
        "io": ["文件读写失败，请检查文件是否存在、被占用或权限。", "File read/write failed; check that the file exists, is not in use and is accessible.", "檔案讀寫失敗，請檢查檔案是否存在、被占用或權限。"],
        "internal": ["内部错误，请复制技术信息反馈。", "Internal error; copy the technical text when reporting it.", "內部錯誤，請複製技術資訊回報。"]
    })

    // Specific messages; first match wins and replaces the generic headline.
    readonly property var messages: ([
        [/ICC colour model does not match output channels/i, ["ICC 色彩模型与输出通道不匹配。请在“曝光、窗口与元数据”中选择“明确丢弃元数据（报告损失）”，或使用匹配的配置文件；删除标记不会执行色彩转换。", "The ICC colour model does not match the output channels. Under Exposure, window and metadata, select Explicitly drop metadata (report losses), or use a matching profile. Dropping the tag does not convert colours.", "ICC 色彩模型與輸出通道不符。請在「曝光、視窗與中繼資料」中選擇「明確捨棄中繼資料（報告損失）」，或使用相符的設定檔；刪除標記不會執行色彩轉換。"]],
        [/glTF inverse bind matrix cannot be retained by FBX cluster API/i, ["当前 FBX 写入器无法准确保留此模型的骨骼绑定矩阵，转换已停止。请在建模软件中重新生成绑定矩阵后重试；原文件未修改。", "The FBX writer cannot accurately retain this model's skin binding matrices. Conversion stopped. Regenerate the bind matrices in your modelling application and retry; the source is unchanged.", "目前 FBX 寫入器無法準確保留此模型的骨骼綁定矩陣，轉換已停止。請在建模軟體中重新產生綁定矩陣後重試；原始檔未修改。"]],
        [/PNG requires explicit straight alpha/i, ["PNG 需要未预乘的 RGB。请在“源 Alpha 类型”中确认直通类型；未知或预乘数据不能直接标成直通输出。", "PNG requires unassociated RGB. Confirm Straight under Source alpha type; unknown or premultiplied data cannot simply be relabeled.", "PNG 需要未預乘的 RGB。請在「來源 Alpha 類型」中確認直通類型；未知或預乘資料不能直接標為直通輸出。"]],
        [/Absent alpha conflicts with selected alpha channel/i, ["图片含有 A 通道，与“无 Alpha”声明冲突。请保留源类型；若要去掉透明度，请选择“删除 Alpha，保留 RGB”。", "The image has an A channel, which conflicts with No alpha. Keep the source type; to remove transparency, choose Drop alpha, keep RGB.", "影像含有 A 通道，與「無 Alpha」宣告衝突。請保留來源類型；若要移除透明度，請選擇「移除 Alpha，保留 RGB」。"]],
        [/Compositing requires explicit straight or premultiplied alpha/i, ["背景合成需要知道 RGB 是否已乘 Alpha。请确认“源 Alpha 类型”为直通或预乘后重试。", "Background compositing requires a known alpha type. Confirm Straight or Premultiplied under Source alpha type and retry.", "背景合成需要知道 RGB 是否已乘 Alpha。請確認「來源 Alpha 類型」為直通或預乘後重試。"]],
        [/Colour conversion requires straight alpha or explicit compositing/i, ["当前颜色运算需要直通 Alpha 或明确的背景合成。请确认源 Alpha 类型，或关闭颜色变换与曝光调整。", "This colour operation requires straight alpha or explicit compositing. Confirm the source alpha type, or disable transfer and exposure adjustments.", "目前色彩運算需要直通 Alpha 或明確的背景合成。請確認來源 Alpha 類型，或關閉色彩變換與曝光調整。"]],
        [/Target alpha removal must be explicit before encoding/i, ["目标格式不支持当前透明通道。请选择“删除 Alpha，保留 RGB”或“在线性背景上合成”。", "The target does not support these alpha channels. Choose Drop alpha, keep RGB or Composite on linear background.", "目標格式不支援目前透明通道。請選擇「移除 Alpha，保留 RGB」或「在線性背景上合成」。"]],
        [/Conversion inputs exceed 4096|Conversion group.*4096/i, ["一次最多转换 4096 张图片。本次未追加，请缩小选择范围。", "A conversion supports up to 4096 images. Nothing was appended; select fewer images.", "一次最多轉換 4096 張影像。本次未追加，請縮小選取範圍。"]],
        [/Conversion inputs exceed 64 source folders/i, ["一次最多读取 64 个来源目录。本次未追加，请减少来源目录。", "A conversion supports up to 64 source folders. Nothing was appended; select fewer folders.", "一次最多讀取 64 個來源資料夾。本次未追加，請減少來源資料夾。"]],
        [/Image import cancelled|Conversion group cancelled/i, ["操作已取消，已完成的输出保留。", "The operation was cancelled; completed outputs are retained.", "操作已取消，已完成的輸出保留。"]],
        [/Duplicate glTF animation target channel|Duplicate glTF animation channel/i, ["同一动画包含重复的节点属性轨道。请修正源文件后重试。", "This animation contains duplicate tracks for the same node property. Correct the source and retry.", "同一動畫包含重複的節點屬性軌道。請修正來源檔後重試。"]],
        [/Invalid glTF sparse.*indices|Sparse animation value read failed/i, ["动画稀疏数据的索引或数值无效，无法安全读取。", "The animation sparse indices or values are invalid and cannot be read safely.", "動畫稀疏資料的索引或數值無效，無法安全讀取。"]],
        [/Animation times must be finite|Animation key count mismatch|Non-finite animation value/i, ["动画关键帧的时间、数量或数值不合法。请检查源动画。", "Animation key times, counts or values are invalid. Check the source animation.", "動畫關鍵影格的時間、數量或數值不合法。請檢查來源動畫。"]],
        [/Cannot open input|Cannot hold parent directory|Cannot inspect file handle/i, ["无法读取文件或目录。请检查路径、访问权限以及文件是否被其他程序占用。", "The file or folder could not be read. Check the path, permissions and whether another application is using it.", "無法讀取檔案或資料夾。請檢查路徑、存取權限及檔案是否被其他程式占用。"]],
        [/Journal already has an owner/i, ["此任务目录已被另一个服务使用。请先关闭该服务再重试。", "Another service is using this task directory. Close that service before retrying.", "此工作目錄已被另一個服務使用。請先關閉該服務再重試。"]],
        [/Journal directory contains unrelated entries|Corrupt journal record|Invalid journal record contract/i, ["任务记录目录含有未知或损坏的文件。文件已保留，请检查记录目录后重试。", "The task record directory contains unknown or damaged files. They were preserved; inspect the directory before retrying.", "工作記錄目錄含有未知或損壞的檔案。檔案已保留，請檢查記錄目錄後重試。"]],
        [/Journal exceeds 100 preserved temporary records/i, ["保留的未提交任务记录过多。请备份并检查记录目录后再启动服务。", "Too many uncommitted task records were preserved. Back up and inspect the directory before restarting the service.", "保留的未提交工作記錄過多。請備份並檢查記錄目錄後再啟動服務。"]],
        [/Input changed after preflight|Preflight approval does not match|approval no longer matches|differs from held snapshot|appeared after preflight|changed\/truncated during read|grew during read/i, ["源文件、参数或授权范围在预检后发生变化。请重新预检后再提交，未写入任何输出。", "The source, parameters or scope changed after preflight. Run a new preflight and submit again; nothing was written.", "來源檔、參數或授權範圍在預檢後發生變化。請重新預檢後再提交，未寫入任何輸出。"]],
        [/Approved (output )?name is occupied|Output rename candidates exhausted/i, ["预检批准的输出名已被占用。请重新预检以分配新的输出名。", "The approved output name is now occupied. Run a new preflight to get a new name.", "預檢核准的輸出名稱已被占用。請重新預檢以配置新的輸出名稱。"]],
        [/Output cannot replace an input|hard link to an input/i, ["输出不能覆盖输入文件（包括硬链接）。请换一个输出位置或名称。", "Output must not replace an input file (including hard links). Choose another location or name.", "輸出不能覆蓋輸入檔（包括硬連結）。請換一個輸出位置或名稱。"]],
        [/reserved by another batch item/i, ["该输出已被同一批次的另一项占用。请改用不同名称或启用重命名。", "Another item in this batch already uses this output. Use a different name or enable rename.", "該輸出已被同一批次的另一項占用。請改用不同名稱或啟用重新命名。"]],
        [/Overwrite permission is not enabled/i, ["未启用覆盖权限。请在输出范围中允许覆盖，或改用重命名/跳过。", "Overwrite is not enabled. Allow overwrite in the output scope, or use rename/skip.", "未啟用覆蓋權限。請在輸出範圍中允許覆蓋，或改用重新命名/略過。"]],
        [/Path is outside the configured scope|Journal directory must belong/i, ["路径不在已配置的输入/输出范围内。请先在设置中添加该目录。", "The path is outside the configured input/output scope. Add the folder in settings first.", "路徑不在已設定的輸入/輸出範圍內。請先在設定中加入該資料夾。"]],
        [/Reparse points are not allowed/i, ["为保护源文件，不允许符号链接/联接点（重解析点）。", "Symbolic links and junctions (reparse points) are not allowed to protect source files.", "為保護來源檔，不允許符號連結/連接點（重新解析點）。"]],
        [/Ambiguous path component/i, ["路径含有空名称、过长名称、‘.’/‘..’或末尾空格/句点。请重命名文件/文件夹或修正路径后重试。", "The path contains an empty/overlong name, '.' or '..', or a trailing space/dot. Rename the file/folder or correct the path, then retry.", "路徑含有空名稱、過長名稱、‘.’/‘..’或末尾空格/句點。請重新命名檔案/資料夾或修正路徑後重試。"]],
        [/Only absolute local drive paths|Mapped network drives|Device\/stream\/wildcard|Reserved device name|Only disk files/i, ["只接受本地磁盘上的绝对路径（不含网络映射盘、设备名或通配符）。", "Only absolute paths on local disks are accepted (no mapped network drives, device names or wildcards).", "只接受本機磁碟上的絕對路徑（不含網路對應磁碟、裝置名稱或萬用字元）。"]],
        [/FBX output already exists/i, ["输出 FBX 已存在，且冲突策略不允许覆盖。", "The output FBX already exists and the conflict policy does not allow overwriting.", "輸出 FBX 已存在，且衝突策略不允許覆蓋。"]],
        [/output directory does not exist/i, ["输出目录不存在。请先创建或选择已有目录。", "The output folder does not exist. Create it or choose an existing folder.", "輸出資料夾不存在。請先建立或選擇既有資料夾。"]],
        [/Cannot commit FBX output|Filesystem cannot prepare a recoverable output commit|Temporary output identity collision|Recovery temporary was replaced/i, ["无法安全提交输出（文件系统不支持可恢复提交或目标被占用）。源文件未被修改。", "The output could not be committed safely (unsupported filesystem or target in use). Source files were not modified.", "無法安全提交輸出（檔案系統不支援可復原提交或目標被占用）。來源檔未被修改。"]],
        [/require(s)? (explicit )?(loss )?acknowledg|losses require/i, ["计划含有有损项。请勾选“已阅读并接受全部损失”后再提交。", "This plan contains losses. Tick “I reviewed and accept all losses” before submitting.", "計畫含有有損項。請勾選「已閱讀並接受全部損失」後再提交。"]],
        [/PSA reference records are placeholders|explicit (reference )?PSK reference is required|supply an explicit reference PSK/i, ["此 PSA 的参考骨架记录只是占位。请选择与之配对的 PSK 作为参考后再导出。", "This PSA's reference records are placeholders. Choose the matching PSK as the reference, then export.", "此 PSA 的參考骨架記錄只是佔位。請選擇與之配對的 PSK 作為參考後再匯出。"]],
        [/requires? (nonempty unique )?(printable )?ASCII/i, ["当前 ActorX→FBX 写出要求骨骼/材质名称为可打印 ASCII；源文件原始字节不受影响。", "The ActorX→FBX writer requires printable ASCII bone/material names; source bytes are unaffected.", "目前 ActorX→FBX 寫出要求骨骼/材質名稱為可列印 ASCII；來源檔原始位元組不受影響。"]],
        [/Negative source weights/i, ["源蒙皮权重含负值，当前写出器不支持，已拒绝以免输出错误数据。", "Source skin weights are negative; the writer does not support them and refused to avoid wrong data.", "來源蒙皮權重含負值，目前寫出器不支援，已拒絕以免輸出錯誤資料。"]],
        [/Morph chunks|morph chunks/i, ["此 ActorX 含形态键数据块，当前写出器不支持，已拒绝。", "This ActorX holds morph chunks that the writer does not support; it was refused.", "此 ActorX 含形態鍵資料區塊，目前寫出器不支援，已拒絕。"]],
        [/\{clip\} needs one selected animation/, ["模板含 {clip}，每个输出只能对应一个所选动作；把多个动作放进同一输出时请改用 {name}-animations.{ext}。", "A {clip} template needs exactly one selected clip per output; for several clips in one output use {name}-animations.{ext}.", "範本含 {clip}，每個輸出只能對應一個所選動作；把多個動作放進同一輸出時請改用 {name}-animations.{ext}。"]],
        [/Output template must be a filename/, ["输出命名模板必须是文件名：不能含路径分隔符或未知占位符，可用 {name}、{clip}、{index}、{ext}。", "The output template must be a plain file name without path separators or unknown placeholders; use {name}, {clip}, {index}, {ext}.", "輸出命名範本必須是檔名：不能含路徑分隔符或未知預留位置，可用 {name}、{clip}、{index}、{ext}。"]],
        [/requires R\/G\/B destination channels/, ["颜色传递/曝光只对 R/G/B 目标通道有定义：请先映射到 RGB，或取消该颜色操作。", "Colour transfer/exposure is defined for R/G/B destination channels only: map to RGB or drop the colour operation.", "顏色傳遞/曝光只對 R/G/B 目標通道有定義：請先映射到 RGB，或取消該顏色操作。"]],
        [/^No source animation selected/, ["未选择任何动作片段：请改为“只导出模型”，或提供 ActorX 动画配对/先勾选片段。", "No animation clips are selected: choose model-only, or supply an ActorX animation pair / tick clips first.", "未選擇任何動作片段：請改為「只匯出模型」，或提供 ActorX 動畫配對／先勾選片段。"]],
        [/^Paired animation could not be inspected/, ["配对的动画文件读取检查失败；真实原因见下方技术信息，未写入任何输出。", "The paired animation could not be inspected; the actual reason is in the technical text below. Nothing was written.", "配對的動畫檔案讀取檢查失敗；真實原因見下方技術資訊，未寫入任何輸出。"]],
        [/requires clip_id\/clip_ids chosen from inspect/, ["导出动画必须先“读取列表”并从中选择动作片段。", "Animation export requires reading the clip list and selecting clips from it first.", "匯出動畫必須先「讀取清單」並從中選擇動作片段。"]],
        [/Select up to 64 inputs|Select between 1 and 64 inputs/i, ["一次最多选择 64 个输入文件（至少 1 个）。", "Select between 1 and 64 input files per plan.", "一次最多選擇 64 個輸入檔（至少 1 個）。"]],
        [/Per-item options must match|Input and per-item options do not match/i, ["每个文件的设置必须与已固定的输入列表一致。请重新打开转换设置。", "Per-item settings must match the fixed input list. Reopen the conversion settings.", "每個檔案的設定必須與已固定的輸入清單一致。請重新開啟轉換設定。"]],
        [/Cancel active work before changing its access scope/i, ["有任务正在运行。请先取消或等待结束，再修改访问范围。", "Work is in progress. Cancel it or wait before changing the access scope.", "有工作正在執行。請先取消或等待結束，再修改存取範圍。"]],
        [/Configure explicit input and output scope first/i, ["请先在设置中明确配置输入与输出范围。", "Configure the input and output scope in settings first.", "請先在設定中明確設定輸入與輸出範圍。"]],
        [/Choose existing absolute Python and adapter paths/i, ["请选择已存在的 Python 与适配器绝对路径，再启用 MCP 端点。", "Choose existing absolute Python and adapter paths, then enable the MCP endpoint.", "請選擇已存在的 Python 與轉接器絕對路徑，再啟用 MCP 端點。"]],
        [/Texture queue is busy/i, ["贴图读取队列繁忙，请稍后重试。", "The texture queue is busy; retry shortly.", "貼圖讀取佇列忙碌，請稍後重試。"]],
        [/no usable UV set for a texture preview/i, ["此网格没有可用于贴图预览的 UV 集。", "This mesh has no usable UV set for a texture preview.", "此網格沒有可用於貼圖預覽的 UV 集。"]],
        [/Request was replaced|result discarded/i, ["请求已被新的操作取代，旧结果已丢弃。", "The request was replaced by a newer action; the old result was discarded.", "請求已被新的操作取代，舊結果已捨棄。"]],
        [/Scheduler is closing/i, ["应用正在关闭，任务未执行。", "The application is closing; the task was not run.", "應用程式正在關閉，工作未執行。"]],
        [/Bounded queue is full/i, ["任务队列已满，请等待现有任务结束后重试。", "The task queue is full; wait for current tasks to finish and retry.", "工作佇列已滿，請等待現有工作結束後重試。"]],
        [/FBX texture has no file path/i, ["FBX 贴图没有文件路径，无法定位。", "The FBX texture has no file path and cannot be located.", "FBX 貼圖沒有檔案路徑，無法定位。"]]
    ])

    // Model/preview notes (warnings), matched by pattern; {1} is the leading name.
    readonly property var notes: ([
        [/^(.*): base colour has no usable UV set/, ["{1}：基础色没有可用的 UV 集；保留了源图片引用，不会臆造坐标。", "{1}: base colour has no usable UV set; the source image reference is kept without inventing coordinates.", "{1}：基礎色沒有可用的 UV 集；保留來源圖片參照，不會臆造座標。"]],
        [/^(.*): source material references absent UV set/, ["{1}：源材质引用了不存在的 UV 集。", "{1}: the source material references a UV set that does not exist.", "{1}：來源材質參照了不存在的 UV 集。"]],
        [/^(.*): base colour references an absent UV set/, ["{1}：基础色引用了不存在的 UV 集。", "{1}: base colour references a UV set that does not exist.", "{1}：基礎色參照了不存在的 UV 集。"]],
        [/^(.*): .* has non-finite or unrepresentable preview coordinates/, ["{1}：某个 UV 集含有非有限或无法表示的预览坐标；原始数值仍保留。", "{1}: a UV set has non-finite or unrepresentable preview coordinates; raw values are kept.", "{1}：某個 UV 集含有非有限或無法表示的預覽座標；原始數值仍保留。"]],
        [/^(.*): invalid source skin weights/, ["{1}：源蒙皮权重无效，已暂停变形；源记录保持不变。", "{1}: source skin weights are invalid; deformation is paused and source records are unchanged.", "{1}：來源蒙皮權重無效，已暫停變形；來源記錄保持不變。"]],
        [/^(.*): preview normalizes derived skin matrices only/, ["{1}：预览仅对派生的蒙皮矩阵归一化；源权重保持原值。", "{1}: preview normalizes derived skin matrices only; raw source weights are unchanged.", "{1}：預覽僅對衍生的蒙皮矩陣正規化；來源權重保持原值。"]],
        [/^(.*): source weights without an active skin binding/, ["{1}：存在源权重但没有生效的蒙皮绑定。", "{1}: source weights exist without an active skin binding.", "{1}：存在來源權重但沒有生效的蒙皮綁定。"]],
        [/^(.*): transparent surfaces use draw-range depth sorting/, ["{1}：透明表面按绘制范围排序，相交的透明面显示可能与其他软件不同。", "{1}: transparent surfaces use draw-range depth sorting; intersecting transparency may differ from other software.", "{1}：透明表面依繪製範圍排序，相交的透明面顯示可能與其他軟體不同。"]],
        [/Reference skeleton NOT applied for this file/, ["此文件未应用参考骨架（原因见技术信息）：按未配对的原始记录预览，层级未经验证。", "The reference skeleton was NOT applied to this file (see technical text): unpaired raw-record preview, hierarchy unverified.", "此檔案未套用參考骨架（原因見技術資訊）：依未配對的原始記錄預覽，層級未經驗證。"]],
        [/Track config NOT applied: it depends on the reference skeleton/, ["轨道 .config 未应用：它依赖参考骨架。", "The track .config was NOT applied: it depends on the reference skeleton.", "軌道 .config 未套用：它依賴參考骨架。"]],
        [/Track config discovered but NOT applied/, ["发现同名轨道 .config，但未应用：请先选择参考 PSK。", "A track .config was found but NOT applied: choose a reference PSK first.", "找到同名軌道 .config 但未套用：請先選擇參考 PSK。"]],
        [/Track config NOT applied:/, ["轨道 .config 未应用（原因见技术信息）；骨架配对仍有效。", "The track .config was NOT applied (see technical text); the skeleton pairing remains in effect.", "軌道 .config 未套用（原因見技術資訊）；骨架配對仍有效。"]],
        [/Animation channel without target node/, ["有动画通道没有目标节点，已跳过。", "An animation channel has no target node and was skipped.", "有動畫通道沒有目標節點，已略過。"]],
        [/Animation clip without usable channels/, ["有动画片段没有可用通道。", "An animation clip has no usable channels.", "有動畫片段沒有可用通道。"]],
        [/Cubic-spline morph weights not supported/, ["形态键权重的三次样条动画不受支持，通道已标记保留。", "Cubic-spline morph-weight animation is not supported; the channel is kept flagged.", "形態鍵權重的三次樣條動畫不受支援，通道已標記保留。"]],
        [/Unsupported animation path/, ["有不受支持的动画路径，已标记保留，不参与播放。", "An unsupported animation path is kept flagged and does not play.", "有不受支援的動畫路徑，已標記保留，不參與播放。"]],
        [/^Verified mapping: (\d+)\/(\d+) bones/, ["骨骼映射已验证：{1}/{2} 个骨骼匹配。", "Mapping verified: {1}/{2} bones matched.", "骨骼對應已驗證：{1}/{2} 個骨骼相符。"]],
        [/^Trial drive \(unverified compatibility\): (\d+)\/(\d+) bones mapped/, ["试播（兼容性未验证）：已映射 {1}/{2} 个骨骼；未映射的目标骨骼保持静态姿势。", "Trial drive (compatibility unverified): {1}/{2} bones mapped; unmapped target bones keep their static pose.", "試播（相容性未驗證）：已對應 {1}/{2} 個骨骼；未對應的目標骨骼保持靜態姿勢。"]],
        [/^Ambiguous bone mapping; drive refused/, ["骨骼名称对应存在歧义，已拒绝驱动，不会静默重定向。", "Bone mapping is ambiguous; the drive was refused and nothing is silently retargeted.", "骨骼名稱對應有歧義，已拒絕驅動，不會靜默重新指定目標。"]],
        [/^Incompatible skeletons; drive refused/, ["骨架不兼容，已拒绝驱动，不会静默重定向。", "The skeletons are incompatible; the drive was refused and nothing is silently retargeted.", "骨架不相容，已拒絕驅動，不會靜默重新指定目標。"]],
        [/^External animation NOT applied \((.*?)\): /, ["外部动画未应用（{1}）；原因见技术信息。", "External animation was NOT applied ({1}); see the technical text for the reason.", "外部動畫未套用（{1}）；原因見技術資訊。"]],
        [/^PSK changed since opening; reopen the model/, ["PSK 在打开后发生了变化；请重新打开模型后再关联 PSA。", "The PSK changed after opening; reopen the model before attaching a PSA.", "PSK 在開啟後發生變化；請重新開啟模型後再關聯 PSA。"]],
        [/^PSA paired with current PSK · (\d+) bones · no track config/, ["PSA 已与当前 PSK 配对 · {1} 个骨骼 · 无轨道 .config", "PSA paired with the current PSK · {1} bones · no track config", "PSA 已與目前 PSK 配對 · {1} 個骨骼 · 無軌道 .config"]],
        [/^PSA paired with current PSK · (\d+) bones · track config applied/, ["PSA 已与当前 PSK 配对 · {1} 个骨骼 · 已应用轨道 .config", "PSA paired with the current PSK · {1} bones · track config applied", "PSA 已與目前 PSK 配對 · {1} 個骨骼 · 已套用軌道 .config"]],
        [/^FBX · source axes preserved · display Y up/, ["FBX · 保留源坐标轴 · 显示为 Y 轴向上", "FBX · source axes preserved · displayed Y-up", "FBX · 保留來源座標軸 · 顯示為 Y 軸向上"]],
        [/^FBX · source axes preserved · (\S+) m\/unit/, ["FBX · 保留源坐标轴 · {1} 米/单位", "FBX · source axes preserved · {1} m/unit", "FBX · 保留來源座標軸 · {1} 公尺/單位"]],
        [/^OBJ · source axes and units unspecified/, ["OBJ · 源坐标轴与单位未声明 · 显示为 Y 轴向上", "OBJ · source axes and units unspecified · displayed Y-up", "OBJ · 來源座標軸與單位未宣告 · 顯示為 Y 軸向上"]],
        [/^ActorX PSA · skeleton preview · Z up/, ["ActorX PSA · 骨架预览 · Z 轴向上", "ActorX PSA · skeleton preview · Z-up", "ActorX PSA · 骨架預覽 · Z 軸向上"]],
        [/^ActorX · source axes\/units unknown/, ["ActorX · 源坐标轴/单位未知 · 显示按 Z 轴向上", "ActorX · source axes/units unknown · displayed Z-up", "ActorX · 來源座標軸/單位未知 · 顯示依 Z 軸向上"]],
        [/^glTF 2\.0 · right-handed · Y up · metres/, ["glTF 2.0 · 右手系 · Y 轴向上 · 单位米", "glTF 2.0 · right-handed · Y-up · metres", "glTF 2.0 · 右手系 · Y 軸向上 · 單位公尺"]]
    ])

    function _t(z: var): string { return Theme.t(z[0], z[1], z[2]) }
    function _fill(text: string, d: string, parts: var): string {
        // Function replacements keep producer-controlled detail strings (which
        // may contain $&, $$ etc.) out of replace()'s special-pattern handling.
        let out = text.replace(/\{d\}/g, () => d)
        for (let i = 1; parts && i < parts.length; ++i) out = out.replace(new RegExp("\\{" + i + "\\}", "g"), () => parts[i] || "")
        return out
    }
    // Split "code: message" produced by services; plain messages have no code.
    function parse(raw: var): var {
        const text = String(raw || "")
        const m = /^([a-z][a-z0-9_]*|[A-Z][A-Z0-9_]*):\s*([\s\S]*)$/.exec(text)
        return m ? { code: m[1], message: m[2], raw: text } : { code: "", message: text, raw: text }
    }
    function lossText(code: string, detail: string): string {
        const entry = losses[code]
        if (!entry) return Theme.t("此转换有损失，未收录说明；请查看技术信息。", "This conversion has a loss without a bundled explanation; see the technical text.", "此轉換有損失，未收錄說明；請查看技術資訊。")
        let d = detail || ""
        if (code === "lossy_codec") {
            d = /JPEG/.test(d) ? Theme.t("JPEG 重新压缩", "JPEG recompression", "JPEG 重新壓縮")
              : /WebP/.test(d) ? Theme.t("WebP 重新压缩", "WebP recompression", "WebP 重新壓縮")
              : /RGBE/.test(d) ? Theme.t("RGBE 共享指数量化", "RGBE shared-exponent quantization", "RGBE 共用指數量化")
              : /BC/.test(d) ? Theme.t("BC 块压缩，BC1 的 Alpha 只有 1 位", "BC block compression; BC1 alpha is 1-bit", "BC 區塊壓縮，BC1 的 Alpha 只有 1 位元") : d
        } else if (code === "omitted_channel") d = d.replace(/^Source channel omitted:\s*/, "")
        else if (code === "metadata_field") d = d.replace(/^Not copied:\s*/, "")
        else if (code === "pmx_nonvertex_morph" || code === "pmx_partial_group_morph") d = d.split(": ")[0]
        return _fill(_t(entry), d, null)
    }
    function lossTechnical(code: string, detail: string): string {
        return code + (detail ? " · " + detail : "")
    }
    function _match(table: var, message: string): var {
        for (const row of table) {
            const hit = row[0].exec(message)
            if (hit) return { text: _fill(_t(row[1]), "", hit) }
        }
        return null
    }
    // Localized explanation of an error string. Empty input gives empty output.
    function errorText(raw: var): string {
        const p = parse(raw)
        if (!p.raw) return ""
        const specific = _match(messages, p.message) || _match(notes, p.message)
        if (specific) return specific.text
        const known = errorCodes[p.code.toLowerCase()]
        if (known) return _t(known)
        return Theme.t("操作未能完成。原因见下方技术信息，可复制后反馈。", "The operation could not be completed. See the technical text below; you can copy it to report.", "操作未能完成。原因見下方技術資訊，可複製後回報。")
    }
    function errorTechnical(raw: var): string { return String(raw || "") }
    // A batch may append " · N of M file(s) NOT applied: names" to a notice.
    function noteText(raw: var): string {
        let text = String(raw || "")
        let suffix = ""
        const tail = /^(.*) · (\d+) of (\d+) file\(s\) NOT applied: (.*)$/.exec(text)
        if (tail) {
            text = tail[1]
            suffix = " · " + Theme.t("%1/%2 个文件未应用：", "%1 of %2 file(s) NOT applied: ", "%1/%2 個檔案未套用：").replace("%1", tail[2]).replace("%2", tail[3]) + tail[4]
        }
        return ((_match(notes, text) || _match(messages, text) || { text: "" }).text || text) + suffix
    }
    function hasNote(raw: var): bool { return !!(_match(notes, String(raw || "")) || _match(messages, String(raw || ""))) }
}
