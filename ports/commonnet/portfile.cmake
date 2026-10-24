# =============================================================================
#  CommonNet 的 vcpkg 端口脚本（逐行注释版）
# -----------------------------------------------------------------------------
#  端口 = 「怎么把这份源码编成包」的说明书，vcpkg 读它来构建并安装 commonnet。
#  本文件与 utils 端口同构（源码定位 / 库类型 / 配置 / 安装 / 附随文件 五段），
#  差异只有两处，都在下面对应位置写明了原因：
#    ① 包名是 CommonNet（大写 C/N），包配置文件装在 lib/cmake/CommonNet；
#    ② 一律编静态库，不跟随三元组的链接方式。
#
#  两种源码来源，自动判定：
#
#  1) 本地开发（本文件位于源码仓库的 ports/commonnet/ 下）——当前使用的方式：
#     直接拿仓库工作区当源码，改完代码重新 install 即可，免 push、免联网、免改 SHA512。
#     注意：改完代码必须「先 remove 再 install --no-binarycaching」，否则 vcpkg 会
#     认为包已装好（它只比对版本号与 ABI 哈希，不看工作区源码）。
#
#  2) 正式发布（把 ports/ 单独发布成 vcpkg 注册表时）：从 GitHub 的 v${VERSION}
#     标签拉取源码并校验 SHA512 —— 该分支尚未端到端验证（开发机访问 GitHub 受限），
#     首次发布时按下方注释补齐 SHA512 后自行验证一次。
#
#  端口可用的预定义变量（由 vcpkg 注入，无需自己定义）：
#     PORT               端口名，这里是 commonnet
#     VERSION            端口版本，取自 vcpkg.json 的 version
#     CURRENT_PACKAGES_DIR 待打包目录，最终会变成 installed/<triplet>/
#     VCPKG_LIBRARY_LINKAGE 链接方式：dynamic 或 static（由三元组决定）
# =============================================================================


# -----------------------------------------------------------------------------
# 1. 取得源码目录
# -----------------------------------------------------------------------------
get_filename_component(_commonnet_port_parent "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE) # 取端口目录的上两级（ports/commonnet → 仓库根）的绝对路径

if(EXISTS "${_commonnet_port_parent}/CMakeLists.txt") # 该路径下若有 CMakeLists.txt，说明端口就放在源码仓库内
    message(STATUS "commonnet: 使用本地源码树 ${_commonnet_port_parent}") # 构建日志里明确提示走了本地分支，便于排查
    set(SOURCE_PATH "${_commonnet_port_parent}") # 直接以仓库根作为源码目录：后续 configure/install 都基于它
else() # 否则说明端口是被单独使用的（例如发布成注册表），需要下载源码
    vcpkg_from_github( # vcpkg 官方下载助手：下载 GitHub 标签归档并校验哈希
        OUT_SOURCE_PATH SOURCE_PATH # 输出变量：解压后的源码目录
        REPO jyoushitou/NetWorkCommunication # 仓库：owner/name 形式
        REF "v${VERSION}" # 取标签 v1.0.0；整体加引号才会展开 ${VERSION}，否则会被当字面量
        SHA512 00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 # 归档的 SHA512 校验值：占位符，发布标签后必须替换
        HEAD_REF main # 仓库默认分支名：供 vcpkg 的版本检查与「最新提交」提示使用
    ) # vcpkg_from_github 结束
    # 上面 SHA512 的真值取法（二选一）：
    #   1) 联网执行 vcpkg install commonnet --overlay-ports=ports --triplet x64-windows
    #      校验失败时 vcpkg 会直接打印「实际是 ...」的真实哈希，复制回来即可；
    #   2) 手动下载 https://github.com/jyoushitou/NetWorkCommunication/archive/refs/tags/v1.0.0.tar.gz
    #      自行计算 SHA512 后填进来。
endif() # 源码来源判定结束


# -----------------------------------------------------------------------------
# 2. 库类型：一律静态
# -----------------------------------------------------------------------------
# utils 端口在这里把 VCPKG_LIBRARY_LINKAGE 翻译成 BUILD_SHARED_LIBS，commonnet 不能
# 照抄：Net 的公开头文件（NetConnection.h / NetServer.h / NetClient.h /
# NetHttpServer.h）里没有任何 __declspec(dllexport) / 导出宏，动态化会编出「导出表为空」
# 的 DLL —— 连导入库都生成不出来，下游链接直接 LNK1104 / LNK2019，而构建期发现不了。
#
# 正确做法是把本端口的链接方式改成 static。
# 不能给 vcpkg_cmake_configure 硬传 -DBUILD_SHARED_LIBS=OFF：helper 会按
# ${VCPKG_LIBRARY_LINKAGE} 自动追加 -DBUILD_SHARED_LIBS=ON，且排在端口 OPTIONS 之后，
# 硬传的 OFF 会被它覆盖（实测 x64-windows 下就在编 DLL，然后 LNK1104 失败）。
# 改这个变量之后，helper 追加的就是 OFF。
#
# 将来若给四个库补上导出宏，删掉下面这一行即可恢复「跟随三元组」的行为。
set(VCPKG_LIBRARY_LINKAGE static)


# -----------------------------------------------------------------------------
# 3. 配置
# -----------------------------------------------------------------------------
# 前置条件：Boost 必须已经装在 vcpkg 里（asio / system / beast / json）。
# 本端口没有把 boost-* 写进 vcpkg.json（理由写在 vcpkg.json 的 $comment 里）：
# 一旦声明，vcpkg 会按端口树的版本（当前 1.92.0）把整棵 Boost 重装一遍，
# 而本机访问 GitHub 超时，boost-cmake 下载三次失败、整个安装直接 BUILD_FAILED。
# 缺少 Boost 时构建会在 Net 自己的 find_package(Boost CONFIG ...) 处停下，
# 报错信息里直接写了要装哪几个端口，不会静默产出坏包。
vcpkg_cmake_configure( # vcpkg 提供的配置助手：自动处理 Debug/Release 两套构建目录与工具链
    SOURCE_PATH "${SOURCE_PATH}" # 源码目录：上面判定出来的路径
    OPTIONS # 传给项目 CMakeLists.txt 的缓存变量
        # 注意：BUILD_SHARED_LIBS 不在这里传 —— helper 会按第 2 节改过的
        # VCPKG_LIBRARY_LINKAGE 自动追加，而且一定排在这批 OPTIONS 之后。
        -DCOMMONNET_BUILD_EXAMPLES=OFF # 端口只出库本体：不编 examples 下的 Server/Client/HttpServer 三个示例
        -DCOMMONNET_BUILD_HTTP_SERVER=ON # 保留 HTTP 服务端库 Common::Net::HttpServer 及其公开头文件（需 boost-beast / boost-json，已在 vcpkg.json 声明）
) # vcpkg_cmake_configure 结束


# -----------------------------------------------------------------------------
# 4. 安装
# -----------------------------------------------------------------------------
vcpkg_cmake_install() # 执行 cmake --install，把产物装进 ${CURRENT_PACKAGES_DIR}（Debug/Release 各一份）

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CommonNet) # 包配置原装位置是 lib/cmake/CommonNet（大小写要与 CMakeLists.txt 里 install(FILES ... DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CommonNet) 一致）；本步骤把它们合并到 share/commonnet，否则 Debug 与 Release 两份会互相覆盖（vcpkg 官方 post-build 检查会报两条警告）；同时把 PACKAGE_PREFIX_DIR 的相对层数从 ../../.. 修正为 ../..（share/commonnet 比 lib/cmake/CommonNet 浅一层）

vcpkg_copy_pdbs() # 收集并安装 pdb 调试符号：本端口编的是静态库，没有 pdb，此命令会自动跳过

file(REMOVE_RECURSE # 删除调试侧多余的副本，保持包内容干净
    "${CURRENT_PACKAGES_DIR}/debug/include" # Debug 版头文件：与 Release 版完全重复，删掉
    "${CURRENT_PACKAGES_DIR}/debug/share" # Debug 版文档与包配置：同样重复，删掉
) # file(REMOVE_RECURSE) 结束


# -----------------------------------------------------------------------------
# 5. 随包附带的使用说明与许可证
# -----------------------------------------------------------------------------
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" # 安装端口自带的使用说明：vcpkg install 结束时会把它打印出来
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}") # 放到 share/commonnet/（PORT 即 commonnet）

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/LICENSE") # 优先用端口目录自带的许可证副本
    file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/LICENSE" # 复制该副本
        DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright) # vcpkg 约定：许可证统一命名为 copyright
else() # 端口目录若没带许可证（例如上游已提供），退回上游文件
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE") # 从源码树取 LICENSE 并安装到 share/${PORT}/copyright
endif() # 许可证安装结束
