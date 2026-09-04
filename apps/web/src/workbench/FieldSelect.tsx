interface FieldSelectProps {
  label: string
  value: string
  fields: string[]
  onChange: (value: string) => void
  optional?: boolean
}

export function FieldSelect({ label, value, fields, onChange, optional = true }: FieldSelectProps) {
  return (
    <label className="field-control">
      <span>{label}</span>
      <select value={value} onChange={(event) => onChange(event.target.value)}>
        <option value="">{optional ? '未映射' : '请选择字段'}</option>
        {fields.map((field) => <option key={field} value={field}>{field}</option>)}
      </select>
    </label>
  )
}
