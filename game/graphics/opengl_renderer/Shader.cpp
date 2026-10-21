#include "Shader.h"

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/graphics/pipelines/opengl.h"

Shader::Shader(const std::string& shader_name,
               GameVersion version,
               const std::string& frag_define,
               const std::string& frag_name)
    : m_name(shader_name) {
  const std::string height_scale = version == GameVersion::Jak1 ? "1.0" : "0.5";
  const std::string scissor_height = version == GameVersion::Jak1 ? "448.0" : "416.0";
  const std::string scissor_adjust = "512.0 / " + scissor_height;

  // read the shader source
  auto vert_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, shader_name + ".vert"}));
  auto frag_src = file_util::read_text_file(file_util::get_file_path(
      {shader_folder, (frag_name.empty() ? shader_name : frag_name) + ".frag"}));
  // 20/10 : etapes de decoupage optionnelles
  const auto tesc_path = file_util::get_file_path({shader_folder, shader_name + ".tesc"});
  const auto tese_path = file_util::get_file_path({shader_folder, shader_name + ".tese"});
  const bool has_tess = file_util::file_exists(tesc_path) && file_util::file_exists(tese_path);
  std::string tesc_src, tese_src;
  if (has_tess) {
    tesc_src = file_util::read_text_file(tesc_path);
    tese_src = file_util::read_text_file(tese_path);
    tese_src = std::regex_replace(tese_src, std::regex("HEIGHT_SCALE"), height_scale);
    tese_src = std::regex_replace(tese_src, std::regex("SCISSOR_HEIGHT"), scissor_height);
    tese_src = std::regex_replace(tese_src, std::regex("SCISSOR_ADJUST"),
                                  "(" + scissor_adjust + ")");
  }

  vert_src = std::regex_replace(vert_src, std::regex("HEIGHT_SCALE"), height_scale);
  vert_src = std::regex_replace(vert_src, std::regex("SCISSOR_HEIGHT"), scissor_height);
  frag_src = std::regex_replace(frag_src, std::regex("SCISSOR_HEIGHT"), scissor_height);
  if (!frag_define.empty()) {
    // apres la ligne #version (elle doit rester la premiere)
    auto nl = frag_src.find('\n');
    frag_src.insert(nl == std::string::npos ? 0 : nl + 1, "#define " + frag_define + "\n");
  }
  vert_src = std::regex_replace(vert_src, std::regex("SCISSOR_ADJUST"), "(" + scissor_adjust + ")");

  m_vert_shader = glCreateShader(GL_VERTEX_SHADER);
  const char* src = vert_src.c_str();
  glShaderSource(m_vert_shader, 1, &src, nullptr);
  glCompileShader(m_vert_shader);

  constexpr int len = 1024;
  int compile_ok;
  char err[len];

  glGetShaderiv(m_vert_shader, GL_COMPILE_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetShaderInfoLog(m_vert_shader, len, nullptr, err);
    lg::error("Failed to compile vertex shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  m_frag_shader = glCreateShader(GL_FRAGMENT_SHADER);
  src = frag_src.c_str();
  glShaderSource(m_frag_shader, 1, &src, nullptr);
  glCompileShader(m_frag_shader);

  glGetShaderiv(m_frag_shader, GL_COMPILE_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetShaderInfoLog(m_frag_shader, len, nullptr, err);
    lg::error("Failed to compile fragment shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  GLuint tess_shaders[2] = {0, 0};
  if (has_tess) {
    const GLenum kinds[2] = {GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER};
    const std::string* srcs[2] = {&tesc_src, &tese_src};
    for (int t = 0; t < 2; t++) {
      tess_shaders[t] = glCreateShader(kinds[t]);
      src = srcs[t]->c_str();
      glShaderSource(tess_shaders[t], 1, &src, nullptr);
      glCompileShader(tess_shaders[t]);
      glGetShaderiv(tess_shaders[t], GL_COMPILE_STATUS, &compile_ok);
      if (!compile_ok) {
        glGetShaderInfoLog(tess_shaders[t], len, nullptr, err);
        lg::error("Failed to compile tessellation shader {}:\n{}", shader_name.c_str(), err);
        m_is_okay = false;
        return;
      }
    }
  }

  m_program = glCreateProgram();
  glAttachShader(m_program, m_vert_shader);
  glAttachShader(m_program, m_frag_shader);
  if (has_tess) {
    glAttachShader(m_program, tess_shaders[0]);
    glAttachShader(m_program, tess_shaders[1]);
  }
  glLinkProgram(m_program);

  glGetProgramiv(m_program, GL_LINK_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetProgramInfoLog(m_program, len, nullptr, err);
    lg::error("Failed to link shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  // uniform samplers must be named matching the texture unit
  glUseProgram(m_program);
  for (int i = 1; i < 30; ++i) {
    std::string uniformName = "tex_T" + std::to_string(i);
    GLint texLoc = glGetUniformLocation(m_program, uniformName.c_str());
    if (texLoc != -1) {
      glUniform1i(texLoc, i);
    }
  }
  // assuming that the bones uniform block is always using binding point 1
  GLint bonesLoc = glGetUniformBlockIndex(m_program, "ub_bones");
  if (bonesLoc != -1) {
    glUniformBlockBinding(m_program, bonesLoc, 1);
  }

  glDeleteShader(m_vert_shader);
  glDeleteShader(m_frag_shader);
  if (has_tess) {
    glDeleteShader(tess_shaders[0]);
    glDeleteShader(tess_shaders[1]);
  }
  m_is_okay = true;
}

void Shader::activate() const {
  ASSERT(m_is_okay);
  glUseProgram(m_program);
}

ShaderLibrary::ShaderLibrary(GameVersion version) {
  at(ShaderId::SOLID_COLOR) = {"solid_color", version};
  at(ShaderId::DIRECT_BASIC) = {"direct_basic", version};
  at(ShaderId::DIRECT_BASIC_TEXTURED) = {"direct_basic_textured", version};
  at(ShaderId::DIRECT_BASIC_TEXTURED_MULTI_UNIT) = {"direct_basic_textured_multi_unit", version};
  at(ShaderId::DEBUG_RED) = {"debug_red", version};
  at(ShaderId::SPRITE) = {"sprite_3d", version};
  at(ShaderId::SKY) = {"sky", version};
  at(ShaderId::SKY_BLEND) = {"sky_blend", version};
  at(ShaderId::TFRAG3) = {"tfrag3", version};
  at(ShaderId::TFRAG3_NO_TEX) = {"tfrag3_no_tex", version};
  at(ShaderId::SPRITE3) = {"sprite3_3d", version};
  at(ShaderId::DIRECT2) = {"direct2", version};
  at(ShaderId::EYE) = {"eye", version};
  at(ShaderId::GENERIC) = {"generic", version};
  at(ShaderId::OCEAN_TEXTURE) = {"ocean_texture", version};
  at(ShaderId::OCEAN_TEXTURE_MIPMAP) = {"ocean_texture_mipmap", version};
  at(ShaderId::OCEAN_COMMON) = {"ocean_common", version};
  at(ShaderId::SHRUB) = {"shrub", version};
  at(ShaderId::SHADOW) = {"shadow", version};
  at(ShaderId::COLLISION) = {"collision", version};
  at(ShaderId::MERC2) = {"merc2", version};
  at(ShaderId::SPRITE_DISTORT) = {"sprite_distort", version};
  at(ShaderId::SPRITE_DISTORT_INSTANCED) = {"sprite_distort_instanced", version};
  at(ShaderId::POST_PROCESSING) = {"post_processing", version};
  at(ShaderId::DEPTH_CUE) = {"depth_cue", version};
  at(ShaderId::EMERC) = {"emerc", version};
  at(ShaderId::GLOW_PROBE) = {"glow_probe", version};
  at(ShaderId::GLOW_PROBE_READ) = {"glow_probe_read", version};
  at(ShaderId::GLOW_PROBE_READ_DEBUG) = {"glow_probe_read_debug", version};
  at(ShaderId::GLOW_PROBE_DOWNSAMPLE) = {"glow_probe_downsample", version};
  at(ShaderId::GLOW_DRAW) = {"glow_draw", version};
  at(ShaderId::ETIE_BASE) = {"etie_base", version};
  at(ShaderId::ETIE) = {"etie", version};
  at(ShaderId::SHADOW2) = {"shadow2", version};
  at(ShaderId::SHADOW_DEPTH) = {"shadow_depth", version};  // PC-REMAKE
  at(ShaderId::MERC_SHADOW) = {"merc_shadow", version};    // PC-REMAKE
  at(ShaderId::TEX_ANIM) = {"tex_anim", version};
  at(ShaderId::GLOW_DEPTH_COPY) = {"glow_depth_copy", version};
  at(ShaderId::GLOW_PROBE_ON_GRID) = {"glow_probe_on_grid", version};
  at(ShaderId::HFRAG) = {"hfrag", version};
  at(ShaderId::HFRAG_MONTAGE) = {"hfrag_montage", version};
  at(ShaderId::PLAIN_TEXTURE) = {"plain_texture", version};
  at(ShaderId::TIE_WIND) = {"tie_wind", version};
  at(ShaderId::SIMPLE_TEXTURE) = {"simple_texture", version};
  at(ShaderId::SLOW_TIME) = {"slow_time", version};
  at(ShaderId::OCEAN_ENVMAP) = {"ocean_envmap", version};
  at(ShaderId::OCEAN_ENVMAP_HAZE) = {"ocean_envmap_haze", version};
  // 20/10 : sol de neige deformable — pixel de tfrag3.frag. Facultatif : si la carte graphique
  // refuse le decoupage, la neige garde le rendu sans deformation (voir TFragment)
  at(ShaderId::TFRAG3_SNOW) = {"tfrag3_snow", version, "", "tfrag3"};
  if (!at(ShaderId::TFRAG3_SNOW).okay()) {
    lg::warn("PC-REMAKE : sol de neige deformable indisponible (decoupage refuse)");
  }

  // 21/10 : personnages remodeles — facultatifs aussi : sans eux, le rendu actuel des
  // personnages (Merc2 retombe sur MERC2 / EMERC)
  at(ShaderId::MERC2_TESS) = {"merc2_tess", version, "", "merc2"};
  at(ShaderId::EMERC_TESS) = {"emerc_tess", version, "", "emerc"};
  at(ShaderId::MERC2_FUR) = {"merc2_fur", version, "FUR", "merc2"};
  for (auto id : {ShaderId::MERC2_TESS, ShaderId::EMERC_TESS, ShaderId::MERC2_FUR}) {
    if (!at(id).okay()) {
      lg::warn("PC-REMAKE : personnages remodeles, shader {} indisponible", (int)id);
    }
  }

  for (int i = 0; i < (int)ShaderId::MAX_SHADERS; i++) {
    if (i == (int)ShaderId::TFRAG3_SNOW || i == (int)ShaderId::MERC2_TESS ||
        i == (int)ShaderId::EMERC_TESS || i == (int)ShaderId::MERC2_FUR) {
      continue;
    }
    ASSERT_MSG(m_shaders[i].okay(), "error compiling shader");
  }
}
