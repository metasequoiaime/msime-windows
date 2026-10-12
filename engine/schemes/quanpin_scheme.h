#pragma once

#include "input_scheme.h"
#include <string>
#include <vector>

class QuanpinScheme : public IInputScheme
{
  public:
    void reset() override;
    void handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch) override;
    QueryRequest build_request() const override;
    std::string get_preedit() const override;
    SchemeType type() const override;
    void set_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases) override;
    // 纠错掩码由会话层在绑定方案时下发（见 ImeSession::configure_quanpin_scheme）。
    // 方案只关心手误别名层开不开（quanpin::correction_aliases_enabled），开关关掉时
    // build_request 的切分只走 ü 归一——关掉开关不再通过别名改写切分。
    void set_autocorrect_types(unsigned autocorrect_types);

  private:
    std::string raw_input_;
    std::vector<KeyStroke> key_strokes_;
    bool include_correction_aliases_ = false;
};
